// CameraController — the single QML-facing object (lives on the GUI thread).
//
// It owns the worker QThread, mirrors device state into Q_PROPERTYs that QML
// binds to, forwards user actions to the worker as queued calls, and persists
// settings/presets. It never makes a blocking SDK call itself.
//
// Design-honesty responsibilities implemented here:
//   * AI toggle revert-on-failure + resync for undecoded modes (CODE_REVIEW #5/#7)
//   * pending-suppression so a status push can't fight an in-flight AI toggle (#8)
//   * capability gating: only expose controls backed by a verified Tiny 3 SDK call
//   * deterministic shutdown of the worker thread (#1/#2/#3/#9)
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QThread>
#include <QVariantList>

#include "Settings.h"

class CameraWorker;
class QTimer;
class QProcess;

class CameraController : public QObject {
    Q_OBJECT

    // ----- connection / identity -----
    Q_PROPERTY(int connState READ connState NOTIFY connStateChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connStateChanged)
    Q_PROPERTY(bool discovering READ discovering NOTIFY connStateChanged)
    Q_PROPERTY(QString product MEMBER m_product NOTIFY identityChanged)
    Q_PROPERTY(QString sn MEMBER m_sn NOTIFY identityChanged)
    Q_PROPERTY(QString firmware MEMBER m_firmware NOTIFY identityChanged)
    Q_PROPERTY(QString mode MEMBER m_mode NOTIFY identityChanged)
    Q_PROPERTY(int enumId MEMBER m_enumId NOTIFY identityChanged)

    // ----- live device state -----
    Q_PROPERTY(int runState READ runState NOTIFY statusChanged)
    Q_PROPERTY(bool asleep READ asleep NOTIFY statusChanged)
    Q_PROPERTY(double zoom MEMBER m_zoom NOTIFY zoomChanged)
    Q_PROPERTY(bool zoomValid MEMBER m_zoomValid NOTIFY zoomChanged)
    Q_PROPERTY(int aiModeRaw MEMBER m_aiModeRaw NOTIFY aiChanged)
    Q_PROPERTY(QString aiModeName MEMBER m_aiModeName NOTIFY aiChanged)
    // AI Track = Human tracking (gimbal follows a person; LED blue). This is the
    // real, working "face/person tracking" on Tiny3. Face focus
    // (cameraSetFaceFocusR, autofocus-only, no gimbal motion) is a separate SDK
    // call, independent of tracking. (A "Face Track"/PortraitTrack mode was tried
    // and removed: on Tiny3 in landscape the device accepted then reverted it —
    // it is portrait-orientation tracking, not face tracking.)
    Q_PROPERTY(bool aiTracking READ aiTracking NOTIFY aiChanged)
    Q_PROPERTY(bool faceFocus READ faceFocus NOTIFY aiChanged)
    // Who AI Track follows: 0=Solo (AiWorkModeHuman), 1=Group (AiWorkModeGroup).
    // Synced from the device's ai_mode while tracking; the next mode otherwise.
    Q_PROPERTY(int trackTarget READ trackTarget WRITE setTrackTarget NOTIFY aiChanged)
    // AI auto-zoom framing: 0=Off, 1=Full body, 2=Upper body, 3=Close-up,
    // 4=Headless, 5=Lower body. Synced from the device's ai_sub_mode while
    // tracking (Off and Upper body share one readback value on Tiny 3).
    Q_PROPERTY(int autoZoom READ autoZoom WRITE setAutoZoom NOTIFY aiChanged)
    Q_PROPERTY(bool gesture READ gesture NOTIFY aiChanged)
    Q_PROPERTY(bool hdrOn READ hdrOn NOTIFY imageChanged)
    Q_PROPERTY(bool aiPending MEMBER m_aiPending NOTIFY aiChanged)

    // ----- persisted app settings (writable from QML) -----
    Q_PROPERTY(int moveStepDeg READ moveStepDeg WRITE setMoveStepDeg NOTIFY settingsChanged)
    Q_PROPERTY(int speedMode READ speedMode WRITE setSpeedMode NOTIFY settingsChanged)
    Q_PROPERTY(int fovIndex READ fovIndex WRITE setFovIndex NOTIFY settingsChanged)
    Q_PROPERTY(int startupPreset READ startupPreset WRITE setStartupPreset NOTIFY settingsChanged)
    Q_PROPERTY(int aiReturnPreset READ aiReturnPreset WRITE setAiReturnPreset NOTIFY settingsChanged)
    Q_PROPERTY(int previewResIndex READ previewResIndex WRITE setPreviewResIndex NOTIFY settingsChanged)
    Q_PROPERTY(QString previewRes READ previewRes NOTIFY settingsChanged)   // human-readable, for STATUS
    Q_PROPERTY(bool sleepOnExit READ sleepOnExit WRITE setSleepOnExit NOTIFY settingsChanged)
    // Experimental, OPT-IN: slow the status cadence while gesture control is on
    // (frequent polling suppresses the camera's gesture recognizer). Off by
    // default so normal behavior is unchanged; kept toggleable for A/B testing
    // across firmware updates.
    Q_PROPERTY(bool gestureLowTraffic READ gestureLowTraffic WRITE setGestureLowTraffic NOTIFY settingsChanged)
    // AI tracking speed, 0=Slow, 1=Standard, 2=Fast: device readback (-1 = unknown).
    Q_PROPERTY(int trackSpeed READ trackSpeed WRITE setTrackSpeed NOTIFY trackSpeedChanged)
    // Device power/sleep behavior. Index 0 = "Device" (don't manage). SDK docs
    // omit the Tiny 3 for both underlying calls — micSleepDevice is the honest
    // device-reported readback (-1 until the first status push).
    Q_PROPERTY(int autoSleepIndex READ autoSleepIndex WRITE setAutoSleepIndex NOTIFY settingsChanged)
    Q_PROPERTY(int micSleepIndex READ micSleepIndex WRITE setMicSleepIndex NOTIFY settingsChanged)
    Q_PROPERTY(int micSleepDevice MEMBER m_micSleepDevice NOTIFY statusChanged)
    Q_PROPERTY(int autoSleepDevice MEMBER m_autoSleepDevice NOTIFY statusChanged)
    Q_PROPERTY(int fps MEMBER m_fps NOTIFY statusChanged)   // current video stream fps

    // ----- app meta -----
    Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

    // ----- image params (0–100, live device values) -----
    Q_PROPERTY(int brightness MEMBER m_brightness NOTIFY imageChanged)
    Q_PROPERTY(int contrast MEMBER m_contrast NOTIFY imageChanged)
    Q_PROPERTY(int saturation MEMBER m_saturation NOTIFY imageChanged)
    Q_PROPERTY(int sharpness MEMBER m_sharpness NOTIFY imageChanged)

    // ----- zone (limit-area) tracking: live device values, degrees -----
    // zoneValid=false until the device answers the zone getters. zoneDef* are
    // the device's default (widest) limits, used as the editing bounds.
    Q_PROPERTY(bool zoneValid MEMBER m_zoneValid NOTIFY zoneChanged)
    Q_PROPERTY(bool zoneTracking MEMBER m_zoneTracking NOTIFY zoneChanged)
    Q_PROPERTY(double zonePanMin MEMBER m_zonePanMin NOTIFY zoneChanged)
    Q_PROPERTY(double zonePanMax MEMBER m_zonePanMax NOTIFY zoneChanged)
    Q_PROPERTY(double zonePitchMin MEMBER m_zonePitchMin NOTIFY zoneChanged)
    Q_PROPERTY(double zonePitchMax MEMBER m_zonePitchMax NOTIFY zoneChanged)
    Q_PROPERTY(double zoneDefPanMin MEMBER m_zoneDefPanMin NOTIFY zoneChanged)
    Q_PROPERTY(double zoneDefPanMax MEMBER m_zoneDefPanMax NOTIFY zoneChanged)
    Q_PROPERTY(double zoneDefPitchMin MEMBER m_zoneDefPitchMin NOTIFY zoneChanged)
    Q_PROPERTY(double zoneDefPitchMax MEMBER m_zoneDefPitchMax NOTIFY zoneChanged)

    // ----- capability gating -----
    Q_PROPERTY(bool capAi READ capAi CONSTANT)
    Q_PROPERTY(bool capFace READ capFace CONSTANT)
    Q_PROPERTY(bool capFov READ capFov CONSTANT)
    Q_PROPERTY(bool capImage READ capImage CONSTANT)      // brightness/contrast/sat/sharp (0–100)
    Q_PROPERTY(bool capGesture READ capGesture CONSTANT)
    Q_PROPERTY(bool capHdr READ capHdr NOTIFY imageChanged)   // dynamic: device reports hdr_support
    Q_PROPERTY(bool capTrackingAdvanced READ capTrackingAdvanced CONSTANT)
    Q_PROPERTY(QString capUnverifiedReason READ capUnverifiedReason CONSTANT)

    // ----- presets + external-preview fallback -----
    Q_PROPERTY(QVariantList presets READ presets NOTIFY presetsChanged)
    // ffplay fallback (kept alongside the embedded preview): external window path
    // for when QtMultimedia misbehaves on a given box.
    Q_PROPERTY(bool previewAvailable READ previewAvailable CONSTANT)

public:
    enum ConnState { Disconnected = 0, Discovering = 1, Connected = 2 };
    Q_ENUM(ConnState)
    enum RunState { RunUnknown = 0, Awake = 1, Asleep = 2 };
    Q_ENUM(RunState)

    explicit CameraController(QObject *parent = nullptr);
    ~CameraController() override;

    void start(int waitMs);   // begin discovery (called from main after QML loads)

    // property getters
    int connState() const { return m_connState; }
    bool connected() const { return m_connState == Connected; }
    bool discovering() const { return m_connState == Discovering; }
    int runState() const { return m_runState; }
    bool asleep() const { return m_runState == Asleep; }
    bool aiTracking() const;
    bool faceFocus() const;
    int trackTarget() const { return m_trackTarget; }
    int autoZoom() const { return m_autoZoom; }
    bool gesture() const;
    bool hdrOn() const { return m_hdrOn; }
    int moveStepDeg() const { return m_settings.moveStepDeg; }
    int speedMode() const { return m_settings.speedMode; }
    int fovIndex() const { return m_settings.fovIndex; }
    int startupPreset() const { return m_settings.startupPreset; }
    int aiReturnPreset() const { return m_settings.aiReturnPreset; }
    int previewResIndex() const { return m_settings.previewResIndex; }
    QString previewRes() const;   // e.g. "1080p60"
    bool sleepOnExit() const { return m_settings.sleepOnExit; }
    bool gestureLowTraffic() const { return m_settings.gestureLowTraffic; }
    int trackSpeed() const { return m_trackSpeed; }
    int autoSleepIndex() const { return m_settings.autoSleepIdx; }
    int micSleepIndex() const { return m_settings.micSleepIdx; }
    QString appVersion() const;

    bool capAi() const { return true; }
    bool capFace() const { return true; }
    bool capFov() const { return true; }
    bool capImage() const { return true; }          // brightness/contrast/sat/sharp: SDK 0–100
    bool capGesture() const { return true; }        // aiSetGestureCtrlIndividualR (rc reported honestly)
    // The Tiny 3 reports hdr_support=0 but applies cameraSetWdrR (readback hdr=1/0).
    bool capHdr() const { return true; }
    bool capTrackingAdvanced() const { return false; }   // framing/sensitivity unverified
    QString capUnverifiedReason() const {
        return QStringLiteral("Not verified against the Tiny 3 SDK — disabled to stay honest.");
    }

    QVariantList presets() const;
    bool previewAvailable() const { return m_previewAvailable; }

public slots:
    // property setters (persist)
    void setMoveStepDeg(int deg);
    void setSpeedMode(int mode);
    void setFovIndex(int idx);
    void setStartupPreset(int p);
    void setAiReturnPreset(int p);
    void setPreviewResIndex(int idx);
    void setSleepOnExit(bool on);
    void setGestureLowTraffic(bool on);
    void setTrackSpeed(int idx);
    void setAutoSleepIndex(int idx);
    void setMicSleepIndex(int idx);
    void resetImageDefaults();     // set brightness/contrast/saturation/sharpness to 50

    // user actions (forwarded to the worker)
    void wake();
    void sleep();
    void center();
    void nudge(int dir);            // 0=up,1=down,2=left,3=right
    void zoomIn();
    void zoomOut();
    void zoomReset();
    void setAiTracking(bool on);      // Human tracking on/off (the real Tiny3 tracking)
    void setTrackTarget(int target);  // 0=Solo, 1=Group; re-sent live if tracking
    void setAutoZoom(int idx);        // see autoZoom property
    void setFaceFocus(bool on);       // face autofocus on/off (independent — no gimbal motion)
    void setGesture(bool on);         // gesture control on/off
    void gestureQuietTest();          // 60 s SDK-traffic pause (gesture diagnostic)
    void setHdr(bool on);             // HDR/WDR on/off (cameraSetWdrR)
    void setImageParam(const QString &param, int value);  // brightness/contrast/saturation/sharpness
    // Zone tracking: set the max zone first, then enable/disable zone tracking.
    void setZoneLimits(double panMin, double panMax, double pitchMin, double pitchMax);
    void resetZoneLimits();          // apply the device's default limits
    void setZoneTracking(bool on);
    // Fill one zone edge ("panMin"/"panMax"/"pitchMin"/"pitchMax") from the
    // camera's current position; result arrives via zoneEdgeCaptured.
    void captureZoneEdge(const QString &edge);
    void rescan();
    void launchPreview();   // FALLBACK: (re)launch the external ffplay preview
    void stopPreview();     // terminate the ffplay preview (also called on shutdown)
    void copyToClipboard(const QString &text);   // e.g. "Copy" on the Log page

    // VELOCITY (hold-to-move) PTZ — see CameraWorker for the safety-stop design.
    // pitchFrac/yawFrac in [-1,1]; scaled internally by the current speed setting.
    void gimbalVelocity(double pitchFrac, double yawFrac);
    void gimbalStop();   // idempotent; also wired to window-blur (stop-on-blur) in Main.qml

    // presets
    void savePreset(int idx);
    void goPreset(int idx);
    void clearPreset(int idx);
    void renamePreset(int idx, const QString &name);
    void saveCurrentToNextEmpty();

signals:
    void connStateChanged();
    void identityChanged();
    void statusChanged();
    void zoomChanged();
    void aiChanged();
    void imageChanged();
    void settingsChanged();
    void presetsChanged();
    void zoneChanged();
    void trackSpeedChanged();
    void zoneEdgeCaptured(const QString &edge, double value);
    void logLine(const QString &kind, const QString &message);
    void commandResult(const QString &action, bool ok, const QString &message);
    void discoveryFinished(bool found);   // one-shot, used by --self-test

private slots:
    void onConnectionResolved(bool found, const QString &product, const QString &sn,
                              const QString &fw, const QString &mode, int enumId);
    void onDeviceLost(const QString &reason);
    void onStatusUpdate(int runState, int aiModeRaw, double zoom, bool zoomValid);
    void onAuxStatus(bool faceFocus, bool hdrOn, bool hdrSupport, int fps, int sleepMicro, int autoSleepSec);
    void onZoomUpdate(double zoom, bool valid);
    void onImageParams(int brightness, int contrast, int saturation, int sharpness);
    void onZoneParams(bool ok, bool tracking, double panMin, double panMax, double pitchMin,
                      double pitchMax, double defPanMin, double defPanMax, double defPitchMin,
                      double defPitchMax);
    void onWorkerResult(const QString &action, bool ok, int rc, const QString &message);
    void onPresetCaptured(int idx, double pitch, double yaw, double zoom, int fov);

private:
    double speedValue() const;   // speedMode -> gimbal reference speed
    QString decodeAiMode(int raw) const;
    void persist();
    // Move to the configured startup preset, after a short delay so the gimbal's
    // power-on self-centering (which runs when the camera connects or wakes)
    // finishes first — otherwise the centering overrides the preset move. `why`
    // is just a log label ("startup" / "wake").
    void scheduleStartupPreset(const QString &why);
    // Delayed, guarded push of the managed power/sleep settings (see impl).
    void applyPowerSettings(const QString &why);
    // Sends AI Track on (in the current trackTarget mode) or off, with the
    // pending/in-flight bookkeeping shared by setAiTracking/setTrackTarget.
    void sendAiTrack(bool on);

    QThread m_thread;
    CameraWorker *m_worker = nullptr;

    int m_connState = Discovering;
    int m_runState = RunUnknown;
    // True once a device has been bound and its startup preset applied; stays
    // true across a still-connected Rescan (re-bind) so the startup preset does
    // NOT re-fire and move the gimbal. Cleared only on a real device loss.
    bool m_hadDevice = false;
    QString m_product, m_sn, m_firmware, m_mode;
    int m_enumId = -1;
    double m_zoom = 1.0;
    bool m_zoomValid = false;
    int m_aiModeRaw = 0;
    QString m_aiModeName = QStringLiteral("Off");

    // Confirmed AI state (what the device actually is, per success/status).
    bool m_aiTracking = false;   // ai_mode == AiWorkModeHuman (owns gimbal)
    int m_trackTarget = 0;       // 0=Solo, 1=Group (see trackTarget property)
    int m_autoZoom = 0;          // see autoZoom property
    bool m_faceFocus = false;    // face autofocus engaged (independent, does NOT own gimbal)
    bool m_gesture = false;      // gesture control (intent; SDK has no clean status readback)
    bool m_hdrOn = false;        // from status tiny.hdr
    bool m_hdrSupport = false;   // from status tiny.hdr_support (drives capHdr)
    // Pending window for the HDR toggle: while set, ignore the ~2-3s status push
    // so it can't clobber the optimistic value before the device applies it
    // (same CODE_REVIEW #8 race the AI-track path guards with m_aiPending).
    bool m_hdrPending = false;
    // Optimistic targets during a toggle (applied on success, discarded on failure).
    bool m_targetTracking = false;
    bool m_targetFace = false;
    bool m_targetGesture = false;
    bool m_targetHdr = false;
    bool m_aiPending = false;
    int m_aiInFlight = 0;        // outstanding AI-track/face-focus toggle legs in flight
    QTimer *m_pendingTimer = nullptr;
    // Started when an AI-Track ON command is confirmed. If the device then
    // disengages on its own shortly after (status push back to None), we log an
    // honest hint — the Tiny 3 accepts the command but silently drops tracking
    // when it can't find a person in view (Rex: "AI track don't work… after
    // some fiddling with presets [re-aiming the camera at me] it worked").
    QElapsedTimer m_aiEngageTime;
    // FOV carried by an in-flight preset recall; applied to the selector in
    // onWorkerResult only when the device accepted the move (never on refusal).
    int m_pendingGoFov = -1;

    // Live image params (0–100), mirrored from the device on connect + on change.
    int m_brightness = 50, m_contrast = 50, m_saturation = 50, m_sharpness = 50;

    // Zone tracking mirror (device readback after every zone command).
    int m_trackSpeed = -1;       // device GimCtrlSpeedMode as UI index (-1 unknown)
    bool m_zoneValid = false;
    bool m_zoneTracking = false;
    double m_zonePanMin = 0, m_zonePanMax = 0, m_zonePitchMin = 0, m_zonePitchMax = 0;
    double m_zoneDefPanMin = 0, m_zoneDefPanMax = 0, m_zoneDefPitchMin = 0, m_zoneDefPitchMax = 0;
    int m_fps = 0;               // current video stream fps (from status)
    int m_micSleepDevice = -1;   // device-reported mic-during-sleep (readback; -1 unknown)
    int m_autoSleepDevice = -1;  // device-reported auto-sleep seconds (readback; -1 unknown, 0 never)

    bool m_previewAvailable = false;
    // Managed ffplay preview process (NOT detached) so it is killed when the app
    // closes — a detached ffplay would linger holding /dev/video0 and spew
    // "/dev/video0: error while seeking" after the app is gone. Also lets a
    // resolution change reload the preview.
    QProcess *m_previewProc = nullptr;

    AppSettings m_settings;
};
