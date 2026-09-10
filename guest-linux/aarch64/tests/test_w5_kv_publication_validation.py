import pathlib
import unittest


class KvPublicationValidationTests(unittest.TestCase):
    def test_accepts_publication_modes_and_rejects_invalid_records(self):
        script = pathlib.Path(__file__).resolve().parents[1] / "scripts/run_llm_infer_eight_node_guest.sh"
        line = next(line for line in script.read_text().splitlines()
                    if "assert_log_has" in line and "stage model_range_kv_state_publish" in line)
        pattern = line.split('"')[3].replace("${idx}", "1").replace("\\\\", "\\")
        record = (
            "[mem_service] stage model_range_kv_state_publish local=node1 step=31 "
            "key=kvcache/qwen3-0-6b/scope/bb94fc9f6bd926dd/node1/layers-0-4/decode-step31 "
            "key_hash=0x8e093f21bebe79ac version=1 layers=[0,4) count=4 "
            "kv_bytes=1114272 kv_checksum=0x80cce3137ca4d4f6 offset=0x3600000 "
            "slot_bytes=2097152 block_bytes=2097152 blocks=1 reserved_bytes=2097152 "
            "producer_publish_ms=1789032236192 epoch=32 seq=32 "
            "backing=obmm_shmem metadata=lingqu_object_service status=ok"
        )
        for fields in ("", "payload_mode=copy publication_copy_bytes=1114272 ",
                       "payload_mode=in_place publication_copy_bytes=0 "):
            with self.subTest(fields=fields):
                sample = record.replace("producer_publish_ms=", fields + "producer_publish_ms=")
                self.assertRegex(sample, pattern)
                self.assertNotRegex(sample.replace("status=ok", "status=failed"), pattern)
                self.assertNotRegex(sample.replace("kv_bytes=1114272", "kv_bytes=0"), pattern)
        self.assertNotRegex(record.replace("producer_publish_ms=", "payload_mode=invalid publication_copy_bytes=0 producer_publish_ms="), pattern)


if __name__ == "__main__":
    unittest.main()
