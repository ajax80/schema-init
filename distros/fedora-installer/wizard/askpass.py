#!/usr/bin/python3
# sudo's SUDO_ASKPASS helper: sudo reads the password from our stdout.
import os
import sys
from PySide6.QtWidgets import QApplication, QInputDialog, QLineEdit


# sudo starts a new askpass for each prompt: only a file can tell this one it is
# a retry. A different prompt is PAM's next question (a PIN, an OTP) after a right
# password, so only the same prompt twice counts. Holds the prompt, never the answer.
def _is_retry(prompt):
    path = os.environ.get("SCHEMA_ASKPASS_TRIES")
    if not path:
        return False
    try:
        with open(path, "r+") as f:
            retry = f.read() == prompt
            f.seek(0)
            f.truncate()
            f.write(prompt)
        return retry
    except OSError:
        return False


def main(argv):
    app = QApplication.instance() or QApplication(argv[:1])
    prompt = argv[1] if len(argv) > 1 else "Password:"
    if _is_retry(prompt):
        prompt = "That password wasn't right. Try again.\n\n" + prompt
    text, ok = QInputDialog.getText(None, "Set up schema", prompt, QLineEdit.Password)
    if not ok:
        return 1
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
