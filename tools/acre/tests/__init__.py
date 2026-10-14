# Tests must never touch the player's real data: point every XDG location at a
# throwaway directory before any test imports acre.config.
import os
import tempfile

_SANDBOX = tempfile.mkdtemp(prefix="acre-tests-")
for _name in ("XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME"):
    os.environ[_name] = os.path.join(_SANDBOX, _name.lower())
