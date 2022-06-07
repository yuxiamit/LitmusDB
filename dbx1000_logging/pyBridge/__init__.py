import sys
import signal
import traceback
sys.path.append('../pequin/compiler/backend/')

from zcc_backend import shortcut
print("pybridge being imported")

def sig_handler(signum, frame):
    traceback.print_exc()
    sys.exit(0)

# signal.signal(signal.SIGSEGV, sig_handler)