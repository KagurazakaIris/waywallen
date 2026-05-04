//! Session-bus presence + control surface for the daemon.

use std::path::Path;
use std::sync::Arc;

use zbus::fdo::RequestNameFlags;
use zbus::names::WellKnownName;
use zbus::{interface, Connection, SignalContext};

use crate::control;
use crate::tasks::TaskState;
use crate::AppState;

pub const BUS_NAME: &str = "org.waywallen.waywallen.Daemon";
pub const OBJECT_PATH: &str = "/org/waywallen/waywallen/Daemon";

pub struct Daemon1 {
    app: Arc<AppState>,
    display_socket_path: String,
}

#[interface(name = "org.waywallen.waywallen.Daemon1")]
impl Daemon1 {
    #[zbus(property)]
    fn display_socket_path(&self) -> &str {
        &self.display_socket_path
    }

    #[zbus(property)]
    fn ws_port(&self) -> u16 {
        self.app.ws_port.load(std::sync::atomic::Ordering::SeqCst)
    }

    #[zbus(property)]
    async fn current_wallpaper_id(&self) -> String {
        self.app
            .playlist
            .lock()
            .await
            .current
            .clone()
            .unwrap_or_default()
    }

    async fn open_ui(&self) -> zbus::fdo::Result<()> {
        if !crate::spawn_ui(&self.app) {
            return Err(zbus::fdo::Error::Failed(
                "waywallen-ui not available".into(),
            ));
        }
        Ok(())
    }

    async fn next(&self) -> zbus::fdo::Result<String> {
        control::step(&self.app, 1)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    async fn previous(&self) -> zbus::fdo::Result<String> {
        control::step(&self.app, -1)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    async fn pause(&self) -> zbus::fdo::Result<()> {
        control::pause_all(&self.app)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    async fn resume(&self) -> zbus::fdo::Result<()> {
        control::resume_all(&self.app)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    async fn rescan(&self) -> zbus::fdo::Result<u32> {
        control::rescan(&self.app)
            .await
            .map(|n| n as u32)
            .map_err(zbus::fdo::Error::from)
    }

    async fn apply_by_id(&self, id: String) -> zbus::fdo::Result<String> {
        control::apply_wallpaper_by_id(&self.app, &id, 0, 0)
            .await
            .map(|r| r.renderer_id)
            .map_err(zbus::fdo::Error::from)
    }

    /// Toggle shuffle on the active playlist. Persisted to settings so
    /// it survives restart.
    async fn set_shuffle(&self, on: bool) {
        control::set_shuffle(&self.app, on).await;
    }

    /// Set the auto-rotation interval in seconds. `0` disables.
    async fn set_rotation_interval(&self, secs: u32) {
        control::set_rotation_interval(&self.app, secs).await;
    }

    /// Activate a persisted playlist by id. Loads its mode/filter and
    /// resolves member ids against the current snapshot.
    async fn activate_playlist(&self, id: i64) -> zbus::fdo::Result<()> {
        control::activate_playlist(&self.app, id)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    /// Switch back to the All pseudo-playlist.
    async fn deactivate_playlist(&self) -> zbus::fdo::Result<()> {
        control::deactivate_playlist(&self.app)
            .await
            .map_err(zbus::fdo::Error::from)
    }

    /// Snapshot of every persisted playlist. Tuple shape
    /// `(id, name, source_kind, mode, interval_secs, item_count)`.
    async fn list_playlists(
        &self,
    ) -> zbus::fdo::Result<Vec<(i64, String, String, String, u32, u32)>> {
        let rows = control::list_playlists(&self.app)
            .await
            .map_err(zbus::fdo::Error::from)?;
        Ok(rows
            .into_iter()
            .map(|s| {
                (
                    s.id,
                    s.name,
                    s.source_kind,
                    s.mode,
                    s.interval_secs.max(0) as u32,
                    s.item_count,
                )
            })
            .collect())
    }

    /// Live status of the active playlist. Tuple shape
    /// `(active_id, mode, interval_secs, current_id, position, count, is_smart)`.
    /// `active_id = 0` and `current_id = ""` represent absence.
    async fn playlist_status(&self) -> (i64, String, u32, String, u32, u32, bool) {
        let s = control::playlist_status(&self.app).await;
        (
            s.active_id.unwrap_or(0),
            s.mode,
            s.interval_secs,
            s.current.unwrap_or_default(),
            s.position.unwrap_or(0),
            s.count,
            s.is_smart,
        )
    }

    fn quit(&self) {
        self.app.shutdown_now();
    }

    /// Snapshot of background tasks tracked by the daemon. Returns one
    /// row per task; rows are not sorted (the registry is a HashMap).
    /// Schema: `a(tssxs)` — (id, kind, name, started_at_ms, state).
    /// `state` is one of `running` / `completed` / `failed` /
    /// `cancelled`. For `failed`, the error message is appended after a
    /// colon (e.g. `failed: nope`) so callers can read it without an
    /// extra round-trip.
    /// Cancel a Running task by id. Returns `true` if a cancel token
    /// existed (the task was Running at call time); `false` if the id
    /// is unknown or the task already finished. The task may take a
    /// few ms to observe the cancellation depending on what it's
    /// awaiting.
    fn cancel_task(&self, id: u64) -> bool {
        self.app.tasks.cancel(id)
    }

    fn list_tasks(&self) -> Vec<(u64, String, String, i64, String)> {
        self.app
            .tasks
            .list()
            .into_iter()
            .map(|r| {
                let state_str = match &r.state {
                    TaskState::Failed(msg) => format!("failed: {msg}"),
                    other => other.as_str().to_string(),
                };
                (
                    r.id,
                    r.kind.as_str().to_string(),
                    r.name,
                    r.started_at_ms,
                    state_str,
                )
            })
            .collect()
    }

    #[zbus(signal)]
    async fn ready(emitter: &SignalContext<'_>) -> zbus::Result<()>;

    #[zbus(signal)]
    async fn shutting_down(emitter: &SignalContext<'_>) -> zbus::Result<()>;
}

/// Single-instance gate. Connects to the session bus and tries to claim
/// `BUS_NAME` with `DO_NOT_QUEUE`. Three outcomes:
///
/// * primary owner ⇒ returns the live `Connection` (caller serves the
///   interface on it later via [`serve`]).
/// * name already taken ⇒ `execv`s into `ui_path`. The current PID
///   becomes the UI process so desktop integration (startup notification
///   cookie, systemd transient unit, taskbar grouping) stays attached to
///   the user's launch. `ui_path = None` ⇒ exits 0.
/// * session bus unavailable / `RequestName` errored ⇒ exits 1. Without
///   a session bus we can't enforce single-instance.
pub async fn acquire_or_handoff(ui_path: Option<&Path>) -> Connection {
    let conn = match Connection::session().await {
        Ok(c) => c,
        Err(e) => {
            eprintln!("waywallen: dbus session bus unavailable: {e}");
            std::process::exit(1);
        }
    };

    let name: WellKnownName<'_> = WellKnownName::try_from(BUS_NAME).expect("valid bus name");
    // zbus 4 maps the `Exists` / `InQueue` reply codes to
    // `Error::NameTaken` / `Error::NameInQueue`; only `PrimaryOwner`
    // and `AlreadyOwner` come back as `Ok`.
    match conn
        .request_name_with_flags(name, RequestNameFlags::DoNotQueue.into())
        .await
    {
        Ok(_) => conn,
        Err(zbus::Error::NameTaken) => {
            let Some(ui) = ui_path else {
                eprintln!("waywallen: already running, no UI to launch");
                std::process::exit(0);
            };
            log::info!("waywallen already running; exec into {}", ui.display());
            use std::os::unix::process::CommandExt;
            let err = std::process::Command::new(ui).exec();
            eprintln!("waywallen: exec {} failed: {err}", ui.display());
            std::process::exit(1);
        }
        Err(e) => {
            eprintln!("waywallen: dbus RequestName failed: {e}");
            std::process::exit(1);
        }
    }
}

/// Publish the `Daemon1` interface on a connection that already owns
/// `BUS_NAME` (returned by [`acquire_or_handoff`]).
pub async fn serve(
    conn: Connection,
    app: Arc<AppState>,
    display_socket_path: String,
) -> zbus::Result<Arc<Connection>> {
    let iface = Daemon1 {
        app,
        display_socket_path,
    };
    conn.object_server().at(OBJECT_PATH, iface).await?;
    Ok(Arc::new(conn))
}

/// Emit the `Ready` signal on the published interface. Safe to call once
/// startup is complete.
pub async fn emit_ready(conn: &Connection) -> zbus::Result<()> {
    let iface_ref = conn
        .object_server()
        .interface::<_, Daemon1>(OBJECT_PATH)
        .await?;
    Daemon1::ready(iface_ref.signal_context()).await
}

/// Emit `ShuttingDown`. Callers should await this before dropping the
/// connection so clients see the signal before the name is released.
pub async fn emit_shutting_down(conn: &Connection) -> zbus::Result<()> {
    let iface_ref = conn
        .object_server()
        .interface::<_, Daemon1>(OBJECT_PATH)
        .await?;
    Daemon1::shutting_down(iface_ref.signal_context()).await
}
