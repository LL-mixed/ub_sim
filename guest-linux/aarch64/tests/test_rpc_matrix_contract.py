from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_four_node_rpc_uses_launcher_serial_ports():
    runner = (ROOT / "scripts" / "run_ub_four_node_rpc_matrix.sh").read_text()
    launcher = (ROOT / "scripts" / "launch_ub_four_node_headless.sh").read_text()

    for node, offset in zip("ABCD", range(16, 20), strict=True):
        variable = f"NODE{node}_SERIAL_PORT"
        assert f"export {variable}='$((PORT_BASE + {offset}))'" in launcher
        assert f'${{{variable}:-$((port_base + {offset}))}}' in runner

    assert "port_base + 31 + idx" not in runner
