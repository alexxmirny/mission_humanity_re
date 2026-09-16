"""Machine-specific default paths for the cfgkit CLIs, sourced from tools/machine_config.py.

The retail INIT.CFG / initlang.cfg live under the unpacked-resources root, which is machine-specific.
Rather than hardcode a machine path in every CLI's argparse default, they import these -- so a new
machine sets the value once (machine_config / MH_RU_CLEAN / machine.local.json), not per script.
"""

import os
import sys

# src/formats/cfgkit/_paths.py -> repo root is four levels up; put tools/ on the path for machine_config.
_REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(_REPO, "tools"))
import machine_config as _m  # noqa: E402

CLEAN = _m.RES_UNPACK  # unpacked pristine resources root
INIT_DEFAULT = CLEAN + "/mh/init/INIT.CFG"
LANG_DEFAULT = CLEAN + "/mh_ex/init/initlang.cfg"
