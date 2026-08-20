#!/usr/bin/env python3
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


class PaperEngramHashHeaderTest(unittest.TestCase):
    def test_guest_c_header_matches_rust_hash_vectors(self):
        cc = shutil.which("cc")
        if cc is None:
            self.skipTest("cc is not available")
        repo = Path(__file__).resolve().parents[3]
        header_dir = repo / "guest-linux" / "aarch64" / "common"
        source = textwrap.dedent(
            r"""
            #include <inttypes.h>
            #include <stdint.h>
            #include <stdio.h>
            #include "paper_engram_hash.h"

            int main(void)
            {
                uint64_t two[] = {1, 2};
                uint64_t two_reversed[] = {2, 1};
                uint64_t three[] = {1, 2, 3};
                uint64_t other_three[] = {10, 11, 12};
                printf("algorithm=%s\n", PAPER_ENGRAM_HASH_ALGORITHM_V1);
                printf("exact_1_2=0x%016" PRIx64 "\n",
                       paper_engram_exact_key_v1(two, 2));
                printf("exact_2_1=0x%016" PRIx64 "\n",
                       paper_engram_exact_key_v1(two_reversed, 2));
                printf("exact_1_2_3=0x%016" PRIx64 "\n",
                       paper_engram_exact_key_v1(three, 3));
                printf("exact_10_11_12=0x%016" PRIx64 "\n",
                       paper_engram_exact_key_v1(other_three, 3));
                printf("row_2_head0=%" PRIu64 "\n",
                       paper_engram_row_hash_v1(2, 0, two, 2, 1024,
                                                UINT64_C(0x12345678)));
                printf("row_2_head1=%" PRIu64 "\n",
                       paper_engram_row_hash_v1(2, 1, two, 2, 1024,
                                                UINT64_C(0x12345678)));
                printf("row_3_head0=%" PRIu64 "\n",
                       paper_engram_row_hash_v1(3, 0, three, 3, 1024,
                                                UINT64_C(0x12345678)));
                return 0;
            }
            """
        )
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / "paper_engram_hash_probe.c"
            bin_path = Path(tmp) / "paper_engram_hash_probe"
            src.write_text(source, encoding="utf-8")
            subprocess.run(
                [
                    cc,
                    "-std=c99",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(header_dir),
                    str(src),
                    "-o",
                    str(bin_path),
                ],
                check=True,
            )
            result = subprocess.run(
                [str(bin_path)], check=True, capture_output=True, text=True
            )

        self.assertEqual(
            result.stdout.strip().splitlines(),
            [
                "algorithm=fnv1a-x64+length-prefix",
                "exact_1_2=0x422dee74521c4b44",
                "exact_2_1=0x122a9fb549f67d24",
                "exact_1_2_3=0xb981081392b03a26",
                "exact_10_11_12=0xeabd6a012d5063ab",
                "row_2_head0=852",
                "row_2_head1=157",
                "row_3_head0=946",
            ],
        )

if __name__ == "__main__":
    unittest.main()
