#!/usr/bin/env python3

from pathlib import PurePath
import errno
import json
import os
import shlex
import shutil
import subprocess
import sys

def destdir_join(d1: str, d2: str) -> str:
    if not d1:
        return d2
    # c:\destdir + c:\prefix must produce c:\destdir\prefix
    return str(PurePath(d1, *PurePath(d2).parts[1:]))

introspect = os.environ.get('MESONINTROSPECT')
out = subprocess.run([*shlex.split(introspect), '--installed'],
                     stdout=subprocess.PIPE, check=True).stdout
for source, dest in json.loads(out).items():
    bundle_dest = destdir_join('qemu-bundle', dest)
    path = os.path.dirname(bundle_dest)
    try:
        os.makedirs(path, exist_ok=True)
    except BaseException as e:
        print(f'error making directory {path}', file=sys.stderr)
        raise e
    try:
        os.symlink(source, bundle_dest)
    except BaseException as e:
        if isinstance(e, OSError) and e.errno == errno.EEXIST:
            continue
        if os.name == 'nt':
            # Without Developer Mode or SeCreateSymbolicLinkPrivilege, Windows
            # refuses symbolic links to an unprivileged user. The bundle only
            # has to mirror the install tree for running from the build
            # directory, so a copy serves as well as a link there.
            # Build outputs do not exist yet at configure time: a link may
            # dangle, a copy cannot, so those are left out of the bundle.
            if os.path.lexists(bundle_dest) or not os.path.exists(source):
                continue
            if os.path.isdir(source):
                shutil.copytree(source, bundle_dest)
            else:
                shutil.copy2(source, bundle_dest)
            continue
        print(f'error making symbolic link {dest}', file=sys.stderr)
        raise e
