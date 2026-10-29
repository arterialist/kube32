//! counter: a request-handling pod with one number in it.
//!
//! Routes. A leading `/api` is ignored, so the same paths work on the pod
//! HTTP server (port 7000) and behind the ingress, which forwards `/api/*`:
//!
//!   GET  /state       {"counter":N,"events_seen":E,"ticks":K,"uptime_ms":T}
//!                     (also GET / on the pod HTTP server)
//!   POST /inc[?by=N]  adds N (default 1), publishes {"type":"counter","value":N}
//!                     on EVENT_TOPIC (default "counter.events") and answers
//!                     {"counter":N,"event":"sent"|"denied"|"failed"}
//!   GET  /env         {"greeting":"<GREETING>"} or {"greeting":null}
//!   anything else     404
//!
//! `events_seen` counts the events on EVENT_TOPIC delivered to `k32_event`,
//! which happens when the pod's manifest lists that topic in the annotation
//! `kube32.io/event-topics`. `event` is "denied" when the manifest does not grant
//! `hardware.kube32.io/ipc` (the capability firewall answers K32_ERR_PERM).
//!
//! Build: cargo build --release --target wasm32-unknown-unknown   (in sdk/rust)
//!    or: tools/kube32-cli build sdk/rust/examples/counter
#![no_std]

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering::Relaxed};
use kube32_sdk::{emit, env, kube32_app, log, parse_u32, time_ms, Buf, Error, Request, Response};

// wasm32 without the atomics feature has no threads; these compile to plain
// loads and stores and need no `unsafe`.
static COUNTER: AtomicU64 = AtomicU64::new(0);
static EVENTS_SEEN: AtomicU32 = AtomicU32::new(0);
static TICKS: AtomicU32 = AtomicU32::new(0);

const DEFAULT_TOPIC: &str = "counter.events";

fn topic(buf: &mut [u8; 64]) -> &str {
    match env("EVENT_TOPIC", buf) {
        Some(t) if !t.is_empty() => t,
        _ => DEFAULT_TOPIC,
    }
}

fn init() -> i32 {
    let mut buf = [0u8; 64];
    let mut line = Buf::<96>::new();
    line.push_str("counter: init, events on ").push_str(topic(&mut buf));
    log(line.as_str());
    0
}

fn handle(req: &Request) -> i32 {
    let path = match req.path.strip_prefix("/api") {
        Some(rest) if rest.starts_with('/') => rest,
        _ => req.path,
    };
    match (req.method, path) {
        ("GET", "/") | ("GET", "/state") => {
            let mut resp = Response::json(200);
            resp.write_str("{\"counter\":")
                .write_u64(COUNTER.load(Relaxed))
                .write_str(",\"events_seen\":")
                .write_u32(EVENTS_SEEN.load(Relaxed))
                .write_str(",\"ticks\":")
                .write_u32(TICKS.load(Relaxed))
                .write_str(",\"uptime_ms\":")
                .write_i64(time_ms())
                .write_str("}");
            resp.end()
        }
        ("POST", "/inc") => {
            let by = match req.query_param("by") {
                None => 1,
                Some(s) => match parse_u32(s) {
                    Some(n) => n,
                    None => {
                        let mut resp = Response::json(400);
                        resp.write_str("{\"error\":\"by must be a number\"}");
                        return resp.end();
                    }
                },
            };
            let value = COUNTER.load(Relaxed).wrapping_add(by as u64);
            COUNTER.store(value, Relaxed);

            // Event payloads are at most 200 bytes: built on the stack.
            let mut ev = Buf::<64>::new();
            ev.push_str("{\"type\":\"counter\",\"value\":").push_u64(value).push_str("}");
            let mut tbuf = [0u8; 64];
            let outcome = match emit(topic(&mut tbuf), ev.as_bytes()) {
                Ok(_) => "sent",
                Err(Error::Permission) => "denied",
                Err(_) => "failed",
            };

            let mut resp = Response::json(200);
            resp.header("Cache-Control", "no-store");
            resp.write_str("{\"counter\":")
                .write_u64(value)
                .write_str(",\"event\":\"")
                .write_str(outcome)
                .write_str("\"}");
            resp.end()
        }
        ("GET", "/env") => {
            let mut buf = [0u8; 64];
            let mut resp = Response::json(200);
            resp.write_str("{\"greeting\":");
            match env("GREETING", &mut buf) {
                Some(v) => resp.write_json_str(v),
                None => resp.write_str("null"),
            };
            resp.write_str("}");
            resp.end()
        }
        _ => {
            let mut resp = Response::json(404);
            resp.write_str("{\"error\":\"not_found\",\"path\":").write_json_str(req.path).write_str("}");
            resp.end()
        }
    }
}

fn tick(_uptime_ms: u32) -> i32 {
    TICKS.fetch_add(1, Relaxed);
    0 // nothing pending
}

fn on_event(t: &str, _data: &[u8]) -> i32 {
    let mut buf = [0u8; 64];
    if t == topic(&mut buf) {
        EVENTS_SEEN.fetch_add(1, Relaxed);
    }
    0
}

kube32_app! {
    init: init,
    handle: handle,
    tick: tick,
    event: on_event,
}
