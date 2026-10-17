"""An IPSW is a ZIP: Restore.plist and BuildManifest.plist name the device, build and components."""
import plistlib
import shutil
import zipfile


class IPSW:
    def __init__(self, path):
        self.path = path
        self.zip = zipfile.ZipFile(path)
        self.names = self.zip.namelist()
        self.restore = plistlib.loads(self.zip.read("Restore.plist"))
        self.manifest = (plistlib.loads(self.zip.read("BuildManifest.plist"))
                         if "BuildManifest.plist" in self.names else {})

    @property
    def build(self):
        return self.restore["ProductBuildVersion"]

    @property
    def version(self):
        return self.restore["ProductVersion"]

    @property
    def product(self):
        return self.restore["SupportedProductTypes"][0]

    @property
    def device_map(self):
        return self.restore.get("DeviceMap", [{}])[0]

    @property
    def board(self):
        return self.device_map.get("BoardConfig")

    def find(self, basename):
        """The member whose last path component is basename (None if absent)."""
        for n in self.names:
            if n.rsplit("/", 1)[-1] == basename:
                return n
        return None

    def read(self, member):
        return self.zip.read(member)

    def size(self, member):
        return self.zip.getinfo(member).file_size

    def extract_to(self, member, dest):
        with self.zip.open(member) as s, open(dest, "wb") as d:
            shutil.copyfileobj(s, d, 1 << 20)

    def components(self):
        """{component: path} as the Restore.plist names them."""
        r = self.restore
        out = {}
        for target, kinds in r.get("KernelCachesByTarget", {}).items():
            for kind, path in kinds.items():
                out[f"kernelcache.{target}.{kind}"] = path
        for kind, path in r.get("RestoreRamDisks", {}).items():
            out[f"ramdisk.{kind}"] = path
        for kind, path in r.get("SystemRestoreImages", {}).items():
            out[f"rootfs.{kind}"] = path
        return out
