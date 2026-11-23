"""make_game.py --pipeline-scope: the pipelines stage checks the main index or only the BVH builder index.

Standard library only; no GPU, no Vulkan SDK, no game data. check(), the checker lookup and every checker process
are mocked; the indexes are synthetic.
"""
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)
import make_game  # noqa: E402

ABORT = 0x80000003   # how Windows reports a driver abort


class PipelineScopeTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = tmp.name
        self.game = os.path.join(self.root, "game")
        self.work = os.path.join(self.root, "work")
        self.out = os.path.join(self.root, "out")
        os.makedirs(self.game)
        self.main_spirv = os.path.join(self.work, "shaders", "spirv")
        self.bvh_spirv = os.path.join(self.main_spirv, "bvh")
        self.calls = []

    # helpers

    def write_index(self, folder, content):
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, "index.json")
        if isinstance(content, bytes):
            with open(path, "wb") as f:
                f.write(content)
        else:
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                f.write(content)

    def write_bytes(self, path, data):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)

    def read_bytes(self, path):
        with open(path, "rb") as f:
            return f.read()

    def fake_run(self, script=lambda args, n: 0):
        """A scripted checker: a finished pass clears its --inflight-file report, as the native one does."""
        def run(args, hint="", check=True, **kw):
            args = [str(a) for a in args]
            self.calls.append(args)
            code = script(args, len(self.calls))
            if code in (0, 1) and "--inflight-file" in args:
                with open(args[args.index("--inflight-file") + 1], "w", encoding="utf-8", newline="\n") as f:
                    f.write("")
            if check and code != 0:
                raise SystemExit(1)
            return code
        return run

    def stage(self, scope, script=lambda args, n: 0, supports=True):
        """Run the real pipelines() for a scope; returns (stderr text, failed)."""
        err = io.StringIO()
        with mock.patch.object(make_game, "pipeline_check_exe", return_value="vk_pipeline_check.exe"), \
                mock.patch.object(make_game, "checker_supports", return_value=supports), \
                mock.patch.object(make_game, "run", side_effect=self.fake_run(script)), \
                mock.patch.object(make_game, "log"), mock.patch("sys.stderr", err):
            try:
                make_game.pipelines(self.work, self.out, 1, pipeline_scope=scope)
                return err.getvalue(), False
            except SystemExit:
                return err.getvalue(), True

    def argv(self, *extra):
        return ["make_game.py", "--game", self.game, "--work", self.work, "--out", self.out,
                "--stages", "pipelines", "--no-tui", *extra]

    # default and argument parsing

    def test_default_scope_keeps_the_main_calls_and_file_names(self):
        with mock.patch.object(sys, "argv", self.argv()), mock.patch.object(make_game, "below_normal"), \
                mock.patch.object(make_game, "check", return_value={"expected_exe": "g.exe"}), \
                mock.patch.object(make_game, "preflight") as preflight, \
                mock.patch.object(make_game, "pipelines") as pipelines, mock.patch.object(make_game, "log"):
            self.assertEqual(make_game.main(), 0)
        self.assertNotIn("pipeline_scope", preflight.call_args.kwargs)
        pipelines.assert_called_once_with(self.work, self.out, mock.ANY, runtime_build=None)
        self.assertEqual(make_game.pipeline_scope_files(self.out),
                         tuple(os.path.join(self.out, n) for n in
                               ("pipeline_cache.bin", "pipeline_inflight.txt", "pipeline_crashes.txt")))
        self.assertEqual(make_game.pipeline_scope_files(self.out, "bvh"),
                         tuple(os.path.join(self.out, n) for n in
                               ("pipeline_cache_bvh.bin", "pipeline_inflight_bvh.txt", "pipeline_crashes_bvh.txt")))

    def test_an_unknown_scope_is_refused_before_anything_runs(self):
        err = io.StringIO()
        with mock.patch.object(sys, "argv", self.argv("--pipeline-scope", "fspx")), \
                mock.patch.object(make_game, "below_normal"), \
                mock.patch.object(make_game, "check") as check, \
                mock.patch.object(make_game, "pipelines") as pipelines, mock.patch("sys.stderr", err):
            with self.assertRaises(SystemExit) as raised:
                make_game.main()
        self.assertEqual(raised.exception.code, 2)
        self.assertIn("--pipeline-scope", err.getvalue())
        check.assert_not_called()
        pipelines.assert_not_called()
        with self.assertRaises(ValueError):
            make_game.pipeline_scope_files(self.out, "fspx")

    # the BVH scope end to end

    def test_bvh_from_the_command_line_checks_only_the_bvh_index(self):
        # No main index exists at all: neither preflight nor the stage may need it.
        self.write_index(self.bvh_spirv, json.dumps({"b_prog": {}, "a_prog": {}}))
        supplied = os.path.join(self.root, "runtime build")
        os.makedirs(supplied)
        lookups = []

        def exe(runtime_build=None):
            lookups.append(runtime_build)
            return "vk_pipeline_check.exe"
        argv = self.argv("--pipeline-scope", "bvh", "--jobs", "1", "--runtime-build", supplied)
        with mock.patch.object(sys, "argv", argv), mock.patch.object(make_game, "below_normal"), \
                mock.patch.object(make_game, "check", return_value={"expected_exe": "g.exe"}), \
                mock.patch.object(make_game, "pipeline_check_exe", side_effect=exe), \
                mock.patch.object(make_game, "checker_supports", return_value=True), \
                mock.patch.object(make_game, "run", side_effect=self.fake_run()), \
                mock.patch.object(make_game, "log"):
            self.assertEqual(make_game.main(), 0)   # the real preflight ran
        self.assertFalse(os.path.exists(os.path.join(self.main_spirv, "index.json")))
        self.assertEqual(lookups[-1], supplied)   # the stage's checker lookup got --runtime-build
        cache, inflight, _ = make_game.pipeline_scope_files(self.out, "bvh")
        self.assertEqual(self.calls, [["vk_pipeline_check.exe", self.bvh_spirv, cache, "--jobs", "1",
                                       "--inflight-file", inflight, "--start", "0", "--count", "2"]])

    def test_preflight_checks_the_selected_index(self):
        self.write_index(self.bvh_spirv, '{"a_prog": {}}')
        with mock.patch.object(make_game, "pipeline_check_exe", return_value="vk_pipeline_check.exe"), \
                mock.patch.object(make_game, "log"):
            make_game.preflight(["pipelines"], self.work, pipeline_scope="bvh")   # no main index needed
            err = io.StringIO()
            with mock.patch("sys.stderr", err), self.assertRaises(SystemExit):
                make_game.preflight(["pipelines"], self.work)   # main still asks for its own
        self.assertIn("No translated shaders yet", err.getvalue())

    def test_a_bad_bvh_index_stops_before_the_checker_without_falling_back(self):
        self.write_index(self.main_spirv, '{"main_mod": {}}')   # a good main index must not be used instead
        index = os.path.join(self.bvh_spirv, "index.json")
        cases = {"missing": None, "a folder": "dir", "not UTF-8": b"\xff\xfe{", "malformed": "{",
                 "a list": '["a_prog"]', "a string": '"a_prog"', "null": "null", "empty": "{}"}
        for label, content in cases.items():
            with self.subTest(label):
                if os.path.isdir(index):
                    os.rmdir(index)
                elif os.path.exists(index):
                    os.remove(index)
                if content == "dir":
                    os.makedirs(index)
                elif content is not None:
                    self.write_index(self.bvh_spirv, content)
                self.calls.clear()
                err, failed = self.stage("bvh")
                self.assertTrue(failed)
                self.assertEqual(self.calls, [])
                self.assertIn("--pipeline-scope bvh", err)
                self.assertIn(index, err)
                with mock.patch.object(make_game, "pipeline_check_exe", return_value="vk_pipeline_check.exe"), \
                        mock.patch("sys.stderr", io.StringIO()) as pre_err, self.assertRaises(SystemExit):
                    make_game.preflight(["pipelines"], self.work, pipeline_scope="bvh")
                self.assertIn(index, pre_err.getvalue())

    # caches and reports

    def test_each_scope_reuses_its_cache_and_leaves_the_other_scope_alone(self):
        self.write_index(self.main_spirv, '{"main_mod": {}}')
        self.write_index(self.bvh_spirv, '{"a_prog": {}}')
        files = {s: make_game.pipeline_scope_files(self.out, s) for s in make_game.PIPELINE_SCOPES}
        for scope, other in (("bvh", "main"), ("main", "bvh")):
            with self.subTest(scope):
                for s, (cache, inflight, skip) in files.items():
                    self.write_bytes(cache, f"{s} cache".encode())
                    self.write_bytes(inflight, f"{s} stale\tmodule\n".encode())
                    self.write_bytes(skip, f"{s}_stale\n".encode())
                before = {p: self.read_bytes(p) for p in files[other]}
                cache = files[scope][0]
                self.calls.clear()
                _, failed = self.stage(scope)
                self.assertFalse(failed)
                self.assertEqual(self.calls[0][2], cache)   # the existing cache is the checker's input
                self.assertEqual(self.read_bytes(cache), f"{scope} cache".encode())
                self.assertFalse(os.path.exists(files[scope][1]))   # scratch reset as before
                self.assertFalse(os.path.exists(files[scope][2]))
                self.assertEqual({p: self.read_bytes(p) for p in files[other]}, before)

    def test_a_checker_failure_in_the_bvh_scope_fails_the_stage(self):
        self.write_index(self.bvh_spirv, '{"a_prog": {}}')
        for code in (1, 2):
            with self.subTest(code=code):
                err, failed = self.stage("bvh", lambda args, n: code)
                self.assertTrue(failed)
                self.assertIn(f"exited with {code}", err)

    def test_a_bvh_crash_is_probed_and_listed_within_the_bvh_scope(self):
        self.write_index(self.bvh_spirv, '{"a_prog": {}, "b_prog": {}}')
        cache, inflight, skip = make_game.pipeline_scope_files(self.out, "bvh")

        def script(args, n):
            if n <= 2:   # the batch, then the probe of the module it names, both crash
                with open(inflight, "w", encoding="utf-8", newline="\n") as f:
                    f.write("b_prog\tpipeline\n")
                return ABORT
            return 1   # the rerun without it: the crashed module counts as failed
        err, failed = self.stage("bvh", script)
        self.assertTrue(failed)
        probe = self.calls[1]
        self.assertEqual(probe[1:3], [self.bvh_spirv, cache])
        self.assertEqual(probe[probe.index("--start") + 1], "1")
        self.assertEqual(self.calls[2][self.calls[2].index("--skip-file") + 1], skip)
        self.assertIn("b_prog", err)

    # stage wiring

    def test_serial_and_overlapped_runs_pass_the_scope_on(self):
        for overlap in (False, True):
            for scope, want in (("main", {"runtime_build": None}),
                                ("bvh", {"runtime_build": None, "pipeline_scope": "bvh"})):
                with self.subTest(overlap=overlap, scope=scope), \
                        mock.patch.object(make_game, "shaders") as shaders, \
                        mock.patch.object(make_game, "pipelines") as pipelines, \
                        mock.patch.object(make_game, "log"):
                    make_game.run_build_stages(["shaders", "pipelines"], game=self.game, work=self.work,
                                               out=self.out, v={}, wav=False, jobs=2, runtime_build=None,
                                               emulate=False, keep_emulated=False, fallbacks=(),
                                               overlap=overlap, pipeline_scope=scope)
                    shaders.assert_called_once()
                    pipelines.assert_called_once()
                    self.assertEqual(pipelines.call_args.args, (self.work, self.out, 2))
                    self.assertEqual(pipelines.call_args.kwargs, want)


if __name__ == "__main__":
    unittest.main()
