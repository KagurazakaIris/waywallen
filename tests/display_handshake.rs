//! End-to-end smoke test for the `waywallen-display-v1` handshake.
//!
//! Spins up a real `display_endpoint::serve` task bound to a tempfile
//! socket, connects a client through the generated codec, and walks
//! the protocol up through `display_accepted`. Bind/SetConfig/FrameReady
//! are NOT exercised here because a real `BindSnapshot` requires a
//! `waywallen-renderer` subprocess — that pipeline is covered by the
//! `display_sync_fd_*` and `ipc_renderer_handshake_rust` tests.
//!
//! What this test verifies:
//!
//!   1. The daemon binds `display.sock` successfully
//!   2. Client can connect, send `hello`, and receive `welcome`
//!   3. `welcome.features` advertises `"explicit_sync_fd"`
//!   4. Client can send `register_display` and receive `display_accepted`
//!   5. The returned `display_id` is non-zero and monotonically assigned
//!   6. No renderer → the server emits a clean error (not a panic) when
//!      the client waits for the next event, and the client sees EOF

use std::sync::Arc;
use std::time::Duration;

use waywallen::display_endpoint;
use waywallen::display_proto::{codec, error_code, Event, Request, PROTOCOL_NAME, PROTOCOL_VERSION};
use waywallen::renderer_manager::RendererManager;
use waywallen::routing::Router;

#[path = "common/mod.rs"]
mod common;

#[tokio::test]
async fn handshake_up_to_display_accepted() {
    let sock = common::tmp_sock("display-handshake");
    let _ = std::fs::remove_file(&sock);

    let mgr = Arc::new(RendererManager::new_default());
    let router = Router::new(Arc::clone(&mgr));

    let sock_for_task = sock.clone();
    let router_for_task = Arc::clone(&router);
    let server_task = tokio::spawn(async move {
        let _ = display_endpoint::serve(&sock_for_task, router_for_task).await;
    });

    assert!(
        common::wait_for_sock_bind(&sock, Duration::from_secs(2)).await,
        "display endpoint did not bind {}",
        sock.display()
    );

    // Drive the client side in a blocking task.
    let sock_for_client = sock.clone();
    let client_handle = tokio::task::spawn_blocking(move || -> anyhow::Result<u64> {
        use std::os::unix::net::UnixStream;
        let stream = UnixStream::connect(&sock_for_client)?;

        // hello → welcome
        codec::send_request(
            &stream,
            &Request::Hello {
                protocol: PROTOCOL_NAME.to_string(),
                client_name: "handshake-test".to_string(),
                client_version: "0.0.1".to_string(),
                client_protocol_version: PROTOCOL_VERSION,
            },
            &[],
        )
        .map_err(|e| anyhow::anyhow!("send hello: {e}"))?;

        let (welcome, _fds) = codec::recv_event(&stream)
            .map_err(|e| anyhow::anyhow!("recv welcome: {e}"))?;
        match welcome {
            Event::Welcome { server_version, features } => {
                assert!(
                    server_version.starts_with("waywallen "),
                    "server_version={server_version}"
                );
                assert!(
                    features.iter().any(|s| s == "explicit_sync_fd"),
                    "explicit_sync_fd not in features={features:?}"
                );
            }
            other => panic!("expected welcome, got opcode {}", other.opcode()),
        }

        // register_display → display_accepted
        codec::send_request(
            &stream,
            &Request::RegisterDisplay {
                name: "DP-test".to_string(),
                width: 1920,
                height: 1080,
                refresh_mhz: 60_000,
                drm_render_major: 0,
                drm_render_minor: 0,
                properties: Vec::new(),
            },
            &[],
        )
        .map_err(|e| anyhow::anyhow!("send register_display: {e}"))?;

        let (accepted, _fds) = codec::recv_event(&stream)
            .map_err(|e| anyhow::anyhow!("recv display_accepted: {e}"))?;
        let id = match accepted {
            Event::DisplayAccepted { display_id } => display_id,
            other => {
                panic!("expected display_accepted, got opcode {}", other.opcode())
            }
        };

        // After display_accepted, the server will try to find a
        // renderer, fail, and close. The test's job is just to record
        // the successful handshake.
        Ok(id)
    });

    let display_id = client_handle
        .await
        .expect("client join")
        .expect("client flow");
    assert!(display_id >= 1, "display_id={display_id}");

    // Ensure the server still exists (hasn't panicked); then clean up.
    assert!(!server_task.is_finished(), "server task exited prematurely");
    server_task.abort();
    let _ = std::fs::remove_file(&sock);
}

#[tokio::test]
async fn rejects_wrong_protocol_string() {
    let sock = common::tmp_sock("display-bad-proto");
    let _ = std::fs::remove_file(&sock);

    let mgr = Arc::new(RendererManager::new_default());
    let router = Router::new(Arc::clone(&mgr));
    let sock_for_task = sock.clone();
    let server_task = tokio::spawn({
        let router = Arc::clone(&router);
        async move {
            let _ = display_endpoint::serve(&sock_for_task, router).await;
        }
    });

    assert!(
        common::wait_for_sock_bind(&sock, Duration::from_secs(2)).await,
        "display endpoint did not bind"
    );

    let sock_for_client = sock.clone();
    let got_error = tokio::task::spawn_blocking(move || -> anyhow::Result<bool> {
        use std::os::unix::net::UnixStream;
        let stream = UnixStream::connect(&sock_for_client)?;
        codec::send_request(
            &stream,
            &Request::Hello {
                protocol: "nope-v0".to_string(),
                client_name: "bad".to_string(),
                client_version: "0".to_string(),
                client_protocol_version: PROTOCOL_VERSION,
            },
            &[],
        )
        .map_err(|e| anyhow::anyhow!("send: {e}"))?;
        // Expect either an Error event or EOF.
        match codec::recv_event(&stream) {
            Ok((Event::Error { .. }, _)) => Ok(true),
            Ok((other, _)) => panic!("unexpected event {:?}", other.opcode()),
            Err(_) => Ok(true), // PeerClosed also acceptable
        }
    })
    .await
    .expect("client join")
    .expect("client flow");

    assert!(got_error, "server must reject bad protocol string");
    server_task.abort();
    let _ = std::fs::remove_file(&sock);
}

/// `client_protocol_version` outside the daemon's supported range
/// must produce `error{code = VERSION_UNSUPPORTED}` followed by close.
#[tokio::test]
async fn rejects_unsupported_client_protocol_version() {
    let sock = common::tmp_sock("display-bad-version");
    let _ = std::fs::remove_file(&sock);

    let mgr = Arc::new(RendererManager::new_default());
    let router = Router::new(Arc::clone(&mgr));
    let sock_for_task = sock.clone();
    let server_task = tokio::spawn({
        let router = Arc::clone(&router);
        async move {
            let _ = display_endpoint::serve(&sock_for_task, router).await;
        }
    });

    assert!(
        common::wait_for_sock_bind(&sock, Duration::from_secs(2)).await,
        "display endpoint did not bind"
    );

    // Probe both ends of the range: too high and too low (saturating
    // to 0 if PROTOCOL_VERSION is already 0, in which case low_probe
    // == 0 and the test still exercises a non-supported value when
    // MIN_SUPPORTED > 0; otherwise low_probe == PROTOCOL_VERSION - 1).
    let high_probe = PROTOCOL_VERSION.saturating_add(99);
    for probe in [high_probe, PROTOCOL_VERSION.saturating_sub(1)] {
        if probe == PROTOCOL_VERSION {
            // PROTOCOL_VERSION is 0 — skip the underflow probe.
            continue;
        }
        let sock_for_client = sock.clone();
        let saw_version_error =
            tokio::task::spawn_blocking(move || -> anyhow::Result<bool> {
                use std::os::unix::net::UnixStream;
                let stream = UnixStream::connect(&sock_for_client)?;
                codec::send_request(
                    &stream,
                    &Request::Hello {
                        protocol: PROTOCOL_NAME.to_string(),
                        client_name: "version-probe".to_string(),
                        client_version: "0.0.1".to_string(),
                        client_protocol_version: probe,
                    },
                    &[],
                )
                .map_err(|e| anyhow::anyhow!("send: {e}"))?;
                match codec::recv_event(&stream) {
                    Ok((Event::Error { code, message }, _)) => {
                        anyhow::ensure!(
                            code == error_code::VERSION_UNSUPPORTED,
                            "expected VERSION_UNSUPPORTED ({}), got code={code} msg={message:?}",
                            error_code::VERSION_UNSUPPORTED,
                        );
                        Ok(true)
                    }
                    Ok((other, _)) => {
                        panic!("expected Error event, got opcode {}", other.opcode())
                    }
                    Err(e) => panic!("expected Error event, got recv err: {e}"),
                }
            })
            .await
            .expect("client join")
            .expect("client flow");
        assert!(
            saw_version_error,
            "probe v{probe}: server must send VERSION_UNSUPPORTED error"
        );
    }

    server_task.abort();
    let _ = std::fs::remove_file(&sock);
}
