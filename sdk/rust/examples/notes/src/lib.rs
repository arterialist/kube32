//! notes: a request-handling pod that keeps a text file in its microSD volume
//! (feature `fs` of kube32-sdk).
//!
//!   GET    /notes   notes.txt as text/plain (404 before the first POST)
//!   POST   /notes   appends the request body and a newline; {"size":N}
//!   DELETE /notes   removes notes.txt; {"removed":true}
//!   GET    /ls      the volume's directory listing, one entry per line
//!
//! A leading `/api` is ignored, as in the counter example. The manifest must
//! request `hardware.kube32.io/sd: "1"` and name a volume claim; without the
//! grant every call answers 403 with the host's error, without a card 503.
//!
//! Build: tools/kube32-cli build sdk/rust/examples/notes
#![no_std]

use kube32_sdk::fs::{self, File};
use kube32_sdk::{kube32_app, log, Error, Request, Response};

const FILE: &str = "notes.txt";

fn fail(e: Error) -> i32 {
    let status = match e {
        Error::Permission => 403,
        Error::NotFound => 404,
        Error::NoDevice => 503,
        Error::NoSpace => 507,
        _ => 500,
    };
    let mut resp = Response::json(status);
    resp.write_str("{\"error\":").write_i64(e.code() as i64).write_str("}");
    resp.end()
}

fn read_notes() -> i32 {
    let mut f = match File::open(FILE) {
        Ok(f) => f,
        Err(e) => return fail(e),
    };
    let mut resp = Response::text(200);
    let mut buf = [0u8; 512];
    loop {
        match f.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                resp.write(&buf[..n]);
            }
            Err(_) => break, // the status line is out; end what was sent
        }
    }
    resp.end()
}

fn append_note(body: &[u8]) -> i32 {
    let mut f = match File::append(FILE) {
        Ok(f) => f,
        Err(e) => return fail(e),
    };
    if let Err(e) = f.write_all(body).and_then(|_| f.write_all(b"\n")) {
        return fail(e);
    }
    let size = match f.size() {
        Ok(n) => n,
        Err(e) => return fail(e),
    };
    if let Err(e) = f.close() {
        return fail(e);
    }
    let mut resp = Response::json(200);
    resp.write_str("{\"size\":").write_u32(size).write_str("}");
    resp.end()
}

fn list() -> i32 {
    let mut buf = [0u8; 1024];
    match fs::list("", &mut buf) {
        Ok(entries) => {
            let mut resp = Response::text(200);
            resp.write(entries);
            resp.end()
        }
        Err(e) => fail(e),
    }
}

fn init() -> i32 {
    log("notes: init");
    0
}

fn handle(req: &Request) -> i32 {
    let path = match req.path.strip_prefix("/api") {
        Some(rest) if rest.starts_with('/') => rest,
        _ => req.path,
    };
    match (req.method, path) {
        ("GET", "/notes") => read_notes(),
        ("POST", "/notes") => append_note(req.body),
        ("DELETE", "/notes") => match fs::remove(FILE) {
            Ok(()) => {
                let mut resp = Response::json(200);
                resp.write_str("{\"removed\":true}");
                resp.end()
            }
            Err(e) => fail(e),
        },
        ("GET", "/ls") => list(),
        _ => {
            let mut resp = Response::json(404);
            resp.write_str("{\"error\":\"not_found\"}");
            resp.end()
        }
    }
}

kube32_app! {
    init: init,
    handle: handle,
}
