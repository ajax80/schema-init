#!/usr/bin/python3
# sudo's SUDO_ASKPASS helper: sudo reads the password from our stdout.
import sys
from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit


def main(argv):
    app = QApplication.instance() or QApplication(argv[:1])
    prompt = argv[1] if len(argv) > 1 else "Password:"
    text, ok = QInputDialog.getText(None, "Set up schema", prompt, QLineEdit.Password)
    if not ok:
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
