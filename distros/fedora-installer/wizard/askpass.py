#!/usr/bin/python3
# sudo's SUDO_ASKPASS helper: sudo reads the password from our stdout.
import os
import sys
from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit


# sudo starts a new askpass for each attempt: only a file can tell this one it
# is a retry. One byte per attempt, never the password.
def _is_retry():
    path = os.environ.get("SCHEMA_ASKPASS_TRIES")
    if not path:
        return False
    try:
        with open(path, "ab+") as f:
            retry = f.tell() > 0
            f.write(b".")
        return retry
    except OSError:
        return False


def main(argv):
    app = QApplication.instance() or QApplication(argv[:1])
    prompt = argv[1] if len(argv) > 1 else "Password:"
    if _is_retry():
        prompt = "That password wasn't right. Try again.\n\n" + prompt
    text, ok = QInputDialog.getText(None, "Set up schema", prompt, QLineEdit.Password)
    if not ok:
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
