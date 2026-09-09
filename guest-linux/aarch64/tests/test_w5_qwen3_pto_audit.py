import importlib.util
import pathlib
import struct
import tempfile
import unittest


SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/audit_w5_qwen3_pto.py"
SPEC = importlib.util.spec_from_file_location("w5_pto_audit", SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def logits(step):
    record = {"step": str(step), "token": "10", "runner_up": "11",
              "candidate_count": "4", "full_vocab_checked": "151936"}
    for i in range(4):
        record.update({f"candidate{i}_token": str(10 + i),
                       f"candidate{i}_logit_bits": hex(struct.unpack("<I", struct.pack("<f", 4 - i))[0]),
                       f"candidate{i}_text_checksum": "0x1234", f"candidate{i}_piece_bytes": "1",
                       f"candidate{i}_piece_word0": "0x61", f"candidate{i}_piece_word1": "0"})
    return record


class W5Qwen3PtoAuditTest(unittest.TestCase):
    def test_logit_gate_rejects_missing_duplicate_wrong_token_and_large_error(self):
        good = [logits(0), logits(1)]
        self.assertEqual(len(MODULE.compare_logits(good, good, 2)), 2)
        for bad in ([logits(0)], [logits(0), logits(0)]):
            with self.assertRaises(ValueError):
                MODULE.compare_logits(bad, good, 2)
        for key, value in (("token", "9"), ("candidate0_logit_bits", "0x40810000"),
                           ("candidate0_logit_bits", "0x7fc00000"), ("candidate2_token", "999"),
                           ("full_vocab_checked", "4")):
            bad = [logits(0), logits(1)]
            bad[0][key] = value
            with self.assertRaises(ValueError):
                MODULE.compare_logits(bad, good, 2)

    def test_complete_audit_requires_actual_completion_kv_and_no_staging(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            candidate, reference = root / "candidate", root / "reference"
            candidate.mkdir()
            reference.mkdir()
            for node, label in enumerate(("nodeA", "nodeB"), 1):
                guest, qemu = [], []
                for step in range(2):
                    tokens, past = (3, 0) if step == 0 else (1, 3)
                    layers = "[0,14)" if node == 1 else "[14,28)"
                    guest += [f"stage w5_qwen3_pto_range_complete node={node} step={step} layers={layers} "
                              f"tokens={tokens} past={past} callable=3 numerical_backend=simpler_pto "
                              f"hidden_bytes={tokens * 2048} kv_bytes={14 * (40 + 8192 * (tokens + past))} "
                              f"kv_offset={1048576 * (step + 1)} status=ok",
                              "[w4_guest] pass",
                              "stage w5_pto_ub_gm_hidden_publish publish_mode=in_place backing=obmm_shmem status=ok"]
                    guest.append(f"stage model_range_kv_state_publish step={step} payload_mode=in_place "
                                 f"publication_copy_bytes=0 offset={1048576 * (step + 1)} "
                                 f"kv_bytes={14 * (40 + 8192 * (tokens + past))} "
                                 f"kv_checksum=0x1234 key_hash=0x5678 version={step + 1} "
                                 "backing=obmm_shmem status=ok")
                    if step:
                        guest.append("stage model_range_kv_state_resolve kv_step=0 offset=1048576 "
                                     f"kv_bytes={14 * (40 + 8192 * 3)} kv_checksum=0x1234 "
                                     "key_hash=0x5678 version=1 backing=obmm_shmem status=ok")
                    request = 0x5157000000000000 | ((node - 1) << 32) | (step + 1)
                    qemu += [f"QEMU_UB_GM_UNBIND request={request} reason=completion_success bindings=6 "
                             f"load_bytes=100 store_bytes=100 fences={node + 1} segment_payload_staging_bytes=0",
                             f"qwen3-pto-range: request={request} shared_payload_staging_bytes=0 status=pass"]
                    counts = MODULE.expected_operator_counts(node, tokens, past)
                    qemu.append(f"QWEN3_PTO_OPERATOR_COUNTS first={(node - 1) * 14} end={node * 14} "
                                f"tokens={tokens} past={past} kernel_complete=1 " +
                                " ".join(f"{k}={v}" for k, v in counts.items()))
                observation = ["stage model_terminal_logits_observation " +
                               " ".join(f"{k}={v}" for k, v in logits(step).items()) for step in range(2)]
                if node == 2:
                    guest += observation
                (candidate / f"{label}_guest.log").write_text("\n".join(guest))
                (candidate / f"{label}_qemu.log").write_text("\n".join(qemu))
                (reference / f"{label}_guest.log").write_text(
                    "[w4_guest] pass\n[w4_guest] pass\n" + "\n".join(observation))
            self.assertEqual(MODULE.audit(candidate, reference, 2, 3)["status"], "pass")
            path = candidate / "nodeA_qemu.log"
            good = path.read_text()
            for source, replacement in (("staging_bytes=0", "staging_bytes=4"),
                                        ("completion_success", "completion_failure"),
                                        ("fences=2", "fences=0"),
                                        ("decoder_layer=14", "decoder_layer=13"),
                                        ("QWEN3_PTO_OPERATOR_COUNTS", "MISSING_OPERATOR_COUNTS")):
                path.write_text(good.replace(source, replacement))
                with self.assertRaises(ValueError):
                    MODULE.audit(candidate, reference, 2, 3)
            path.write_text(good)
            path = candidate / "nodeA_guest.log"
            good = path.read_text()
            for source, replacement in (("payload_mode=in_place", "payload_mode=copy"),
                                        ("publication_copy_bytes=0", "publication_copy_bytes=64"),
                                        ("kv_offset=1048576", "kv_offset=1048640"),
                                        ("kv_step=0 offset=1048576", "kv_step=0 offset=1048640"),
                                        ("stage model_range_kv_state_publish", "missing_kv_publish")):
                path.write_text(good.replace(source, replacement))
                with self.assertRaises(ValueError):
                    MODULE.audit(candidate, reference, 2, 3)
            path.write_text(good)
            path.write_text(path.read_text() + "\n[w4_guest] fail numerical error")
            with self.assertRaises(ValueError):
                MODULE.audit(candidate, reference, 2, 3)


if __name__ == "__main__":
    unittest.main()
