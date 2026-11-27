"""Reject duplicate YAML keys and malformed embedded Bash in release workflows."""
from pathlib import Path
import subprocess
import re
from graphlib import TopologicalSorter
import yaml

class UniqueLoader(yaml.SafeLoader):
    pass

def mapping(loader, node, deep=False):
    result = {}
    for key_node, value_node in node.value:
        key = loader.construct_object(key_node, deep=deep)
        if key in result:
            raise ValueError(f"duplicate YAML key {key!r} at line {key_node.start_mark.line + 1}")
        result[key] = loader.construct_object(value_node, deep=deep)
    return result

UniqueLoader.add_constructor(yaml.resolver.BaseResolver.DEFAULT_MAPPING_TAG, mapping)
for path in (Path(".github/workflows/release.yml"), Path(".github/workflows/version-release.yml")):
    data = yaml.load(path.read_text(), Loader=UniqueLoader)
    assert isinstance(data.get("jobs"), dict) and data["jobs"], path
    jobs = data["jobs"]
    dependencies = {}
    for name, job in jobs.items():
        needs = job.get("needs", [])
        needs = [needs] if isinstance(needs, str) else needs
        assert isinstance(needs, list) and all(n in jobs for n in needs), f"{path}: unknown dependency for {name}"
        dependencies[name] = needs
    tuple(TopologicalSorter(dependencies).static_order())
    for name, job in jobs.items():
        if "uses" in job:
            continue
        assert job.get("steps"), f"{path}: empty job {name}"
        for step in job["steps"]:
            run = step.get("run")
            shell = step.get("shell", job.get("defaults", {}).get("run", {}).get("shell", data.get("defaults", {}).get("run", {}).get("shell")))
            runner = job.get("runs-on", "")
            unix = isinstance(runner, str) and runner.startswith(("ubuntu", "macos"))
            if not run or not (shell == "bash" or (shell is None and unix)):
                continue
            # Substitute Actions expressions before parsing shell source.
            run = re.sub(r"\$\{\{.*?\}\}", "ACTION_EXPRESSION", run, flags=re.S)
            subprocess.run(["bash", "-n"], input=run, text=True, check=True)
    print(f"{path}: duplicate-key, job dependency and Bash syntax checks passed")
