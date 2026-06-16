from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def test_ub_chat_is_packaged_from_app_directory():
    build_script = (ROOT / "scripts" / "build_initramfs.sh").read_text()

    assert 'CHAT_SRC="$ROOT_DIR/apps/ub_chat/ub_chat.c"' in build_script
    assert not (ROOT / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "ub_chat.c").exists()
    assert (ROOT / "apps" / "ub_chat" / "Makefile").exists()
