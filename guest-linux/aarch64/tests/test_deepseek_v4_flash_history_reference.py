"""Model-free CLI tests; fake logits here are never integration evidence."""

import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


HEADER = r"""
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef struct { const char *model_path; int backend, n_threads, context_size,
    mtp_draft_tokens; float mtp_margin; } ds4_engine_options;
typedef struct { int unused; } ds4_engine;
typedef struct { int pos; } ds4_session;
enum { DS4_BACKEND_CPU };
int ds4_engine_open(ds4_engine **, const ds4_engine_options *);
int ds4_session_create(ds4_session **, ds4_engine *, int);
int ds4_session_eval(ds4_session *, int, char *, size_t);
int ds4_session_pos(ds4_session *);
int ds4_session_copy_logits(ds4_session *, float *, int);
int ds4_session_save_payload(ds4_session *, FILE *, char *, size_t);
void ds4_session_free(ds4_session *);
void ds4_engine_close(ds4_engine *);
"""
STUB = r"""
#include "ds4.h"
#include <math.h>
#include <string.h>
static ds4_engine engine;
static ds4_session session;
static const char *mode;
int ds4_engine_open(ds4_engine **out, const ds4_engine_options *o) {
    mode = o->model_path; *out = &engine; return 0;
}
int ds4_session_create(ds4_session **out, ds4_engine *e, int ctx) {
    (void)e; (void)ctx; *out = &session; return 0;
}
int ds4_session_eval(ds4_session *s, int token, char *err, size_t size) {
    (void)err; (void)size;
    if (token != (s->pos ? s->pos : 42)) return 1;
    ++s->pos; return 0;
}
int ds4_session_pos(ds4_session *s) { return s->pos; }
int ds4_session_copy_logits(ds4_session *s, float *out, int n) {
    memset(out, 0, n * sizeof(float)); out[s->pos] = 1;
    if (!strcmp(mode, "nonfinite")) out[0] = NAN;
    return n;
}
int ds4_session_save_payload(ds4_session *s, FILE *f, char *err, size_t size) {
    (void)err; (void)size;
    return !strcmp(mode, "save-fail") ||
           fwrite(&s->pos, sizeof(s->pos), 1, f) != 1;
}
void ds4_session_free(ds4_session *s) { (void)s; }
void ds4_engine_close(ds4_engine *e) { (void)e; }
"""


class HistoryReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler unavailable")
        cls.build = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.build.cleanup)
        root = Path(cls.build.name)
        (root / "ds4.h").write_text(HEADER)
        (root / "stub.c").write_text(STUB)
        cls.binary = root / "history-reference"
        subprocess.run([
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
            "-I", str(root),
            str(Path(__file__).with_name("deepseek_v4_flash_history_reference.c")),
            str(root / "stub.c"), "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def test_cli_help_and_invalid_arguments(self):
        self.assertEqual(subprocess.run(
            [self.binary, "--help"], capture_output=True).returncode, 0)
        for steps, token in [("0", "42"), ("4junk", "42"),
                             ("4", "-1"), ("4", "129280")]:
            with self.subTest(steps=steps, token=token):
                result = subprocess.run([self.binary, "model", steps, token,
                                         "/unused", "/unused"], capture_output=True)
                self.assertEqual(result.returncode, 2)

    def test_every_position_uses_previous_model_logits(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "state"
            trace = Path(directory) / "trace"
            result = subprocess.run([self.binary, "model", "4", "42", payload,
                                     trace], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            records = [json.loads(line) for line in trace.read_text().splitlines()]
            self.assertEqual([r["position"] for r in records], list(range(4)))
            self.assertEqual([r["input"] for r in records], [42, 1, 2, 3])
            self.assertEqual([r["next"] for r in records], [1, 2, 3, 4])
            self.assertTrue(payload.is_file())

    def test_failures_do_not_leave_valid_looking_payload(self):
        for mode in ("nonfinite", "save-fail"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                payload = Path(directory) / "state"
                result = subprocess.run([self.binary, mode, "4", "42", payload,
                                         Path(directory) / "trace"],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 1)
                self.assertFalse(payload.exists())

    def test_existing_payload_is_never_overwritten(self):
        with tempfile.TemporaryDirectory() as directory:
            payload = Path(directory) / "state"
            payload.write_bytes(b"existing")
            result = subprocess.run([self.binary, "model", "4", "42", payload,
                                     Path(directory) / "trace"], capture_output=True)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(payload.read_bytes(), b"existing")


if __name__ == "__main__":
    unittest.main()
