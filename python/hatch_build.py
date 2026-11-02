"""Build hook: the package carries files that live elsewhere in the repository.

include/runtime/k32_app_abi.h is the single copy of the pod ABI, which
`kube32 build` compiles against. A build from the repository copies it into
the package as kube32/_data/<path>; a build from the sdist uses the copy
the sdist carries under src/kube32/_data/. The repository's
LICENSE goes into the sdist and into the wheel's .dist-info/licenses/.
"""
import os

from hatchling.builders.hooks.plugin.interface import BuildHookInterface

# repository path -> path below kube32/_data/ (the same table as kube32.common.SHARED_FILES)
SHARED_FILES = {
    "include/runtime/k32_app_abi.h": "include/runtime/k32_app_abi.h",
}


def shared_sources(project_root, target):
    """{source file: path in the archive} for the build `target` ("wheel" or "sdist")."""
    repo = os.path.dirname(os.path.abspath(project_root))
    prefix = "src/kube32/_data/" if target == "sdist" else "kube32/_data/"
    out = {}
    for repo_path, data_path in SHARED_FILES.items():
        in_repo = os.path.join(repo, *repo_path.split("/"))
        in_sdist = os.path.join(project_root, "src", "kube32", "_data", *data_path.split("/"))
        if os.path.isfile(in_repo):
            out[in_repo] = prefix + data_path
        elif not os.path.isfile(in_sdist):
            raise FileNotFoundError("neither %s nor %s exists" % (in_repo, in_sdist))
        # else: a build from the sdist, whose package directory holds the file already
    return out


class CustomBuildHook(BuildHookInterface):
    PLUGIN_NAME = "custom"

    def initialize(self, version, build_data):
        build_data["force_include"].update(shared_sources(self.root, self.target_name))
        if os.path.isfile(os.path.join(self.root, "LICENSE")):
            return   # a build from the sdist: hatchling finds LICENSE itself
        repo_license = os.path.join(os.path.dirname(os.path.abspath(self.root)), "LICENSE")
        if self.target_name == "sdist":
            build_data["force_include"][repo_license] = "LICENSE"
        else:   # a wheel built from the repository without an sdist
            build_data["extra_metadata"][repo_license] = "licenses/LICENSE"
