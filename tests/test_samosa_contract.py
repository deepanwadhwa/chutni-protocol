#!/usr/bin/env python3
"""The native service contract consumed by Samosa's compiled gateway."""

import json
import os
from pathlib import Path
import subprocess
import tempfile


MCP = os.environ.get("CHUTNI_MCP", "build/chutni-mcp")


def call(name, arguments, env):
    completed = subprocess.run(
        [MCP, "--call", name, json.dumps(arguments, separators=(",", ":"))],
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )
    assert completed.returncode == 0, (
        name,
        completed.returncode,
        completed.stdout,
        completed.stderr,
    )
    assert not completed.stderr, completed.stderr
    result = json.loads(completed.stdout)
    assert result.get("ok") is not False, result
    return result


def main():
    with tempfile.TemporaryDirectory(prefix="chutni-samosa-contract-") as tmp:
        base = Path(tmp)
        home = base / "home"
        root = base / "Research"
        root.mkdir()
        note = root / "notes.txt"
        note.write_text(
            "The condensation force is twelve piconewtons.\n",
            encoding="utf-8",
        )
        (root / "sample.bin").write_bytes(b"\x00\xff\x01")
        root_path = str(root.resolve())
        note_path = str(note.resolve())
        env = os.environ.copy()
        env["HOME"] = str(home)
        env["CHUTNI_HOME"] = str(home / "chutni")
        env.pop("CHUTNI_STORE", None)

        status = call("chutni_folder_status", {"path": root_path}, env)
        assert status["action"] == "create_store"
        store = status["store_path"]
        assert store == f"{root_path}.chutni"

        activated = call(
            "chutni_folder_activate",
            {
                "path": root_path,
                "confirmed": True,
                "register": False,
                "label": "Research",
                "app_name": "samosa",
                "app_version": "compat-test",
            },
            env,
        )
        assert activated["store_path"] == store
        assert activated["scan"]["files_seen"] == 2
        assert activated["scan"]["sources_indexed"] == 2

        counts = call("chutni_store_info", {"store_path": store}, env)["counts"]
        required_counts = {
            "content_artifacts",
            "metadata_artifacts",
            "content_readable_sources",
            "metadata_only_sources",
            "sources_files",
            "artifacts_active",
        }
        assert required_counts <= counts.keys(), counts
        assert counts["content_artifacts"] == 1
        assert counts["content_readable_sources"] == 1
        assert counts["metadata_only_sources"] == 1

        inventory = call(
            "chutni_list_sources",
            {
                "store_path": store,
                "source_path": root_path,
                "limit": 80,
                "offset": 0,
            },
            env,
        )
        assert inventory["count"] == 2
        assert inventory["returned"] == 2
        assert {Path(item["display_path"]).name for item in inventory["sources"]} == {
            "notes.txt",
            "sample.bin",
        }
        assert all(
            {"source_id", "display_path", "media_type", "state", "size_bytes"}
            <= item.keys()
            for item in inventory["sources"]
        )

        found = call(
            "chutni_search",
            {"store_path": store, "query": "condensation force", "limit": 6},
            env,
        )
        assert found["count"] == 1
        hit = found["results"][0]
        assert {
            "source_id",
            "artifact_id",
            "display_path",
            "snippet",
            "freshness",
        } <= hit.keys()
        assert hit["freshness"] == "current"

        derived = call(
            "chutni_put_derived_artifact",
            {
                "store_path": store,
                "source_path": note_path,
                "text": "A parser retained the measured force.",
                "artifact_kind": "page_text",
                "producer_name": "samosa-reader",
                "producer_version": "1",
                "app_name": "samosa",
                "app_version": "compat-test",
                "confirmed": True,
            },
            env,
        )
        assert derived["artifact_id"]

        model = call(
            "chutni_put_model_artifact",
            {
                "store_path": store,
                "source_path": note_path,
                "text": "The note records a force measurement.",
                "model_id": "samosa/test-model",
                "model_revision": "r1",
                "app_name": "samosa",
                "app_version": "compat-test",
                "confirmed": True,
            },
            env,
        )
        assert model["artifact_id"]

        print("Samosa compatibility contract: 7 operations passed")


if __name__ == "__main__":
    main()
