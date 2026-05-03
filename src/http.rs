use std::ffi::CString;
use std::sync::{Once, OnceLock};
use crate::c;
use bch_bindgen::printbuf::Printbuf;

extern crate tiny_http;

fn http_thread(server: tiny_http::Server) {
    use tiny_http::{Response};

    for request in server.incoming_requests() {
        let (_, path) = request.url().split_once('/').unwrap();

        let c_path = CString::new(path).unwrap();

        match request.method() {
            tiny_http::Method::Get => {
                let mut buf = Printbuf::new();

                let ret = unsafe { c::sysfs_read_or_html_dirlist(c_path.as_ptr(), buf.as_raw()) };

                if ret < 0 {
                    let response = Response::from_string(format!("Error {}", ret))
                        .with_status_code(403);
                    request.respond(response).expect("Responded");
                } else {
                    let response = Response::from_string(buf.as_str());
                    request.respond(response).expect("Responded");
                }
            }

            _ => {
                let response = Response::from_string("Unsupported HTTP method")
                    .with_status_code(405);
                request.respond(response).expect("Responded");
            }
        };
    }
}

/*
 * Pick a per-process unix socket path: /run/bcachefs/<pid>.sock for root,
 * $XDG_RUNTIME_DIR/bcachefs/<pid>.sock (typically /run/user/<uid>/...)
 * for unprivileged callers. Caller is responsible for ensuring the
 * parent dir exists (see ensure_socket_dir).
 */
fn http_socket_path() -> String {
    let pid = std::process::id();
    let uid = unsafe { libc::geteuid() };
    let parent = if uid == 0 {
        "/run/bcachefs".to_string()
    } else if let Some(dir) = std::env::var_os("XDG_RUNTIME_DIR") {
        format!("{}/bcachefs", dir.to_string_lossy())
    } else {
        format!("/run/user/{}/bcachefs", uid)
    };
    format!("{}/{}.sock", parent, pid)
}

static SOCKET_PATH: OnceLock<String> = OnceLock::new();

extern "C" fn cleanup_socket() {
    if let Some(path) = SOCKET_PATH.get() {
        let _ = std::fs::remove_file(path);
    }
}

/*
 * Bind a unix socket and spawn a thread serving sysfs/debugfs over HTTP.
 * Idempotent: safe to call from every fs-start path; only the first call
 * actually starts the server.
 *
 * Called from linux/kobject.c's debugfs_create_file shim, so userspace
 * fses (mount, fsck, format, migrate, ...) all expose their debugfs
 * tree without needing to opt in.
 *
 * Cleanup is via an atexit handler that unlinks the socket on normal
 * exit. Sockets from killed-by-signal / panicked processes are left
 * behind; user can rm them. Bind will fail loudly if a stale file
 * exists rather than overwrite it (so we never hijack a path that
 * something else is using).
 */
#[no_mangle]
pub extern "C" fn bch2_start_http_lazy() {
    static INIT: Once = Once::new();
    INIT.call_once(|| {
        let path = http_socket_path();
        if let Some(parent) = std::path::Path::new(&path).parent() {
            let _ = std::fs::create_dir_all(parent);
        }

        match tiny_http::Server::http_unix(std::path::Path::new(&path)) {
            Ok(server) => {
                let _ = SOCKET_PATH.set(path);
                unsafe { libc::atexit(cleanup_socket); }
                std::thread::spawn(move || http_thread(server));
            }
            Err(e) => {
                eprintln!("bcachefs: failed to bind {}: {}", path, e);
            }
        }
    });
}
