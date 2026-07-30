#!/usr/bin/env python3
"""End-to-end checks for the reusable Chutni MCP/one-shot service."""

import json
import os
import pathlib
import subprocess
import tempfile


MCP = os.environ.get("CHUTNI_MCP", "build/chutni-mcp")


class McpSession:
    def __init__(self, env):
        self.proc = subprocess.Popen(
            [MCP, "--stdio"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env,
        )
        self.next_id = 1

    def request(self, method, params=None):
        request_id = self.next_id
        self.next_id += 1
        message = {
            "jsonrpc": "2.0",
            "id": request_id,
            "method": method,
        }
        if params is not None:
            message["params"] = params
        self.proc.stdin.write(json.dumps(message, separators=(",", ":")) + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        assert line, f"server closed stdout; stderr={self.proc.stderr.read()}"
        response = json.loads(line)
        assert response["id"] == request_id
        return response

    def tool(self, name, arguments=None):
        response = self.request(
            "tools/call",
            {"name": name, "arguments": arguments or {}},
        )
        assert "result" in response, response
        result = response["result"]
        structured = result.get("structuredContent")
        assert isinstance(structured, dict), result
        return structured, bool(result.get("isError"))

    def close(self):
        self.proc.stdin.close()
        status = self.proc.wait(timeout=5)
        stderr = self.proc.stderr.read()
        assert status == 0, stderr
        assert not stderr, stderr


def one_shot(name, arguments, env, expected_status=0):
    completed = subprocess.run(
        [MCP, "--call", name, json.dumps(arguments, separators=(",", ":"))],
        text=True,
        capture_output=True,
        env=env,
        check=False,
    )
    assert completed.returncode == expected_status, completed.stderr
    assert not completed.stderr, completed.stderr
    return json.loads(completed.stdout)


def assertion_count():
    """How many checks main() actually runs.

    Every assertion in main() is unconditional, so reaching the end means all
    of them passed. Counting the source keeps the reported number from drifting
    away from the work, which is exactly what a hardcoded tally does.
    """
    import ast

    tree = ast.parse(pathlib.Path(__file__).read_text(encoding="utf-8"))
    body = next(
        node
        for node in tree.body
        if isinstance(node, ast.FunctionDef) and node.name == "main"
    )
    return sum(isinstance(node, ast.Assert) for node in ast.walk(body))


def main():
    with tempfile.TemporaryDirectory(prefix="chutni-mcp-") as temporary:
        root = pathlib.Path(temporary)
        source = root / "Research"
        source.mkdir()
        note = source / "notes.txt"
        note.write_text(
            "Marsupials carry their young in a pouch.\n",
            encoding="utf-8",
        )
        home = root / "chutni-home"
        env = os.environ.copy()
        env["CHUTNI_HOME"] = str(home)
        # CHUTNI_HOME only relocates the registry. §39 discovery also sweeps
        # the conventional locations under $HOME, so without this the test
        # counts the developer's own stores as if the suite had made them.
        env["HOME"] = str(home)
        env.pop("CHUTNI_STORE", None)

        session = McpSession(env)

        initialized = session.request(
            "initialize",
            {
                "protocolVersion": "2025-11-25",
                "capabilities": {},
                "clientInfo": {"name": "conformance-client", "version": "1"},
            },
        )
        assert initialized["result"]["protocolVersion"] == "2025-11-25"
        assert initialized["result"]["serverInfo"]["name"] == "chutni-mcp"

        discovered_protocol = session.request(
            "server/discover",
            {
                "_meta": {
                    "io.modelcontextprotocol/protocolVersion": "2026-07-28",
                    "io.modelcontextprotocol/clientInfo": {
                        "name": "conformance-client",
                        "version": "1",
                    },
                    "io.modelcontextprotocol/clientCapabilities": {},
                }
            },
        )
        assert "2026-07-28" in discovered_protocol["result"]["supportedVersions"]

        listed = session.request("tools/list", {})
        names = {tool["name"] for tool in listed["result"]["tools"]}
        expected = {
            "chutni_folder_status",
            "chutni_folder_activate",
            "chutni_discover",
            "chutni_capabilities",
            "chutni_store_info",
            "chutni_scan",
            "chutni_search",
            "chutni_children",
            "chutni_observe_directory",
            "chutni_coverage",
            "chutni_source_context",
            "chutni_put_artifacts",
            "chutni_put_model_artifact",
        }
        assert names == expected

        capabilities, failed = session.tool("chutni_capabilities", {})
        assert not failed
        assert capabilities["semantic_validation"] == "not_performed"
        assert capabilities["writer_policy"] == "single_writer_many_readers"
        assert capabilities["reference_scanner"]["file_metadata_for_every_file"]
        assert not capabilities["reference_scanner"]["ocr"]

        status, failed = session.tool(
            "chutni_folder_status", {"path": str(source)}
        )
        assert not failed
        assert status["action"] == "create_store"
        store = pathlib.Path(status["store_path"])
        assert store == pathlib.Path(f"{source.resolve()}.chutni")

        refused, failed = session.tool(
            "chutni_folder_activate",
            {"path": str(source), "confirmed": False},
        )
        assert failed and refused["error"] == "confirmation_required"
        assert not store.exists()

        activated, failed = session.tool(
            "chutni_folder_activate",
            {
                "path": str(source),
                "confirmed": True,
                "register": True,
                "label": "Research",
                "app_name": "test-host",
                "app_version": "1.0",
            },
        )
        assert not failed, activated
        assert activated["created"] is True
        assert activated["store_path"] == str(store)
        assert activated["scan"]["text_artifacts"] == 1
        assert activated["scan"]["metadata_artifacts"] == 1
        # The note's two artifacts, plus the root directory's own listing and
        # the coverage manifest for this scan generation (§15.5, §15.7).
        assert activated["counts"]["artifacts_active"] == 4
        assert activated["scan"]["directories_enumerated"] == 1
        assert activated["scan"]["complete_for_policy"] is True
        assert (store / "manifest.json").is_file()
        assert (store / "catalog.sqlite").is_file()

        initial_context, failed = session.tool(
            "chutni_source_context",
            {"store_path": str(store), "source_path": str(note)},
        )
        assert not failed, initial_context
        assert initial_context["artifact_count"] == 2
        assert {
            artifact["artifact_kind"]
            for artifact in initial_context["artifacts"]
        } == {"file_metadata", "extracted_text"}
        assert all(
            artifact["provenance"]["derivation"]["operation"]
            in {"record_file_metadata", "extract_text"}
            for artifact in initial_context["artifacts"]
        )

        found, failed = session.tool(
            "chutni_search",
            {
                "store_path": str(store),
                "query": "marsupials pouch",
                "limit": 5,
            },
        )
        assert not failed
        assert found["count"] == 1
        assert found["results"][0]["display_path"] == str(note.resolve())
        assert found["results"][0]["freshness"] == "current"

        broad, failed = session.tool(
            "chutni_search",
            {
                "store_path": str(store),
                "query": "unmatched marsupials",
                "match_any": True,
            },
        )
        assert not failed
        assert broad["count"] == 1

        info, failed = session.tool(
            "chutni_store_info", {"store_path": str(store)}
        )
        assert not failed
        assert info["counts"]["roots"] == 1
        # The note, plus the root directory itself as a source of its own.
        assert info["counts"]["sources"] == 2
        assert info["counts"]["sources_files"] == 1
        assert info["counts"]["sources_directories"] == 1
        assert info["counts"]["sources_opaque_directories"] == 0
        assert info["counts"]["artifacts_active"] == 4

        discovered, failed = session.tool("chutni_discover", {})
        assert not failed
        assert discovered["count"] == 1
        assert discovered["stores"][0]["store_path"] == str(store)

        note.write_text(
            "Cephalopods change color using chromatophores.\n",
            encoding="utf-8",
        )
        scanned, failed = session.tool(
            "chutni_scan",
            {
                "store_path": str(store),
                "confirmed": True,
                "app_name": "test-host",
                "app_version": "1.0",
            },
        )
        assert not failed, scanned
        assert scanned["scan"]["text_artifacts"] == 1

        old, failed = session.tool(
            "chutni_search",
            {"store_path": str(store), "query": "marsupials pouch"},
        )
        assert not failed and old["count"] == 0

        current, failed = session.tool(
            "chutni_search",
            {"store_path": str(store), "query": "chromatophores"},
        )
        assert not failed and current["count"] == 1

        current_context, failed = session.tool(
            "chutni_source_context",
            {"store_path": str(store), "source_path": str(note)},
        )
        assert not failed, current_context
        current_hash = current_context["source"]["content_hash"]

        refused, failed = session.tool(
            "chutni_put_artifacts",
            {
                "store_path": str(store),
                "source_path": str(note),
                "source_content_hash": current_hash,
                "producer": {
                    "producer_kind": "parser",
                    "name": "Host A OCR",
                    "version": "3",
                    "app_name": "host-a",
                    "app_version": "1",
                },
                "operation": "ocr",
                "artifacts": [
                    {
                        "artifact_kind": "ocr_text",
                        "artifact_origin": "deterministic_transform",
                        "text": "OCR page one mentions chromatophores.",
                        "selector": {"type": "pages", "start": 1, "end": 1},
                    }
                ],
                "confirmed": False,
            },
        )
        assert failed and refused["error"] == "confirmation_required"

        parsed, failed = session.tool(
            "chutni_put_artifacts",
            {
                "store_path": str(store),
                "source_path": str(note),
                "source_content_hash": current_hash,
                "producer": {
                    "producer_kind": "parser",
                    "name": "Host A OCR",
                    "version": "3",
                    "app_name": "host-a",
                    "app_version": "1",
                },
                "operation": "ocr",
                "recipe_hash": "recipe:ocr-v3",
                "parameters": {"language": "en"},
                "artifacts": [
                    {
                        "artifact_kind": "ocr_text",
                        "artifact_origin": "deterministic_transform",
                        "text": "OCR page one mentions chromatophores.",
                        "selector": {"type": "pages", "start": 1, "end": 1},
                    },
                    {
                        "artifact_kind": "ocr_text",
                        "artifact_origin": "deterministic_transform",
                        "text": "OCR page two mentions adaptive color.",
                        "selector": {"type": "pages", "start": 2, "end": 2},
                    },
                ],
                "confirmed": True,
            },
        )
        assert not failed, parsed
        assert len(parsed["artifacts"]) == 2
        assert parsed["semantic_validation"] == "not_performed"

        interpreted, failed = session.tool(
            "chutni_put_artifacts",
            {
                "store_path": str(store),
                "source_id": current_context["source"]["source_id"],
                "source_content_hash": current_hash,
                "producer": {
                    "producer_kind": "model",
                    "name": "Host B local model",
                    "model_id": "example/host-b-model",
                    "model_revision": "r2",
                    "runtime": "host-b-runtime",
                    "app_name": "host-b",
                    "app_version": "2",
                },
                "operation": "summarize",
                "recipe_hash": "recipe:summary-r2",
                "inputs": [
                    {
                        "artifact_id": parsed["artifacts"][0]["artifact_id"],
                        "role": "ocr_text",
                    }
                ],
                "artifacts": [
                    {
                        "artifact_kind": "summary_short",
                        "artifact_origin": "model_generated",
                        "text": "This file is about a dog.",
                    }
                ],
                "confirmed": True,
            },
        )
        assert not failed, interpreted

        wrong_version, failed = session.tool(
            "chutni_put_artifacts",
            {
                "store_path": str(store),
                "source_path": str(note),
                "source_content_hash": "blake3:" + ("0" * 64),
                "producer": {
                    "producer_kind": "parser",
                    "name": "Host C parser",
                },
                "operation": "extract",
                "artifacts": [
                    {
                        "artifact_kind": "page_text",
                        "artifact_origin": "deterministic_transform",
                        "text": "must not be stored",
                    }
                ],
                "confirmed": True,
            },
        )
        assert failed and wrong_version["error"] == "source_version_mismatch"

        incomplete_model, failed = session.tool(
            "chutni_put_artifacts",
            {
                "store_path": str(store),
                "source_path": str(note),
                "source_content_hash": current_hash,
                "producer": {
                    "producer_kind": "parser",
                    "name": "Unidentified model pipeline",
                },
                "operation": "summarize",
                "artifacts": [
                    {
                        "artifact_kind": "summary_short",
                        "artifact_origin": "model_generated",
                        "text": "must not be stored without model identity",
                    }
                ],
                "confirmed": True,
            },
        )
        assert failed and incomplete_model["ok"] is False

        combined, failed = session.tool(
            "chutni_source_context",
            {
                "store_path": str(store),
                "source_id": current_context["source"]["source_id"],
            },
        )
        assert not failed, combined
        by_kind = {}
        for artifact in combined["artifacts"]:
            by_kind.setdefault(artifact["artifact_kind"], []).append(artifact)
        assert len(by_kind["ocr_text"]) == 2
        assert any(
            artifact["content"] == "This file is about a dog."
            and artifact["provenance"]["producer"]["app_name"] == "host-b"
            and artifact["provenance"]["derivation"]["operation"] == "summarize"
            and artifact["semantic_validation"] == "not_performed"
            for artifact in by_kind["summary_short"]
        )

        stored, failed = session.tool(
            "chutni_put_model_artifact",
            {
                "store_path": str(store),
                "source_path": str(note),
                "text": "The note describes adaptive camouflage.",
                "artifact_kind": "summary_short",
                "model_id": "example/local-model",
                "model_revision": "revision-1",
                "runtime": "test-runtime",
                "app_name": "test-host",
                "app_version": "1.0",
                "operation": "summarize",
                "recipe_hash": "recipe:test-v1",
                "parameters": {"max_tokens": 64},
                "confirmed": True,
            },
        )
        assert not failed, stored
        assert stored["artifact_id"]
        assert stored["semantic_validation"] == "not_performed"

        summary, failed = session.tool(
            "chutni_search",
            {"store_path": str(store), "query": "adaptive camouflage"},
        )
        assert not failed and summary["count"] == 1
        assert summary["results"][0]["artifact_id"] == stored["artifact_id"]

        # ------------------------------------------- bounded coverage (§11.1)
        #
        # A second folder, deep enough that a depth bound actually bites, so
        # the service can be checked on the distinction it exists to preserve:
        # what was opened versus what was only named.
        bounded_source = root / "Library"
        (bounded_source / "Papers" / "Drafts").mkdir(parents=True)
        (bounded_source / "Photos").mkdir()
        (bounded_source / "index.txt").write_text(
            "Top level index of the library.\n", encoding="utf-8"
        )
        (bounded_source / "Papers" / "paper.txt").write_text(
            "A paper about hydrothermal vents.\n", encoding="utf-8"
        )
        (bounded_source / "Papers" / "Drafts" / "draft.txt").write_text(
            "A draft about tardigrade cryptobiosis.\n", encoding="utf-8"
        )

        bounded, failed = session.tool(
            "chutni_folder_activate",
            {
                "path": str(bounded_source),
                "confirmed": True,
                "max_depth": 1,
                "memory_goal": "define",
                "definition_mode": "adaptive",
                "app_name": "test-host",
                "app_version": "1.0",
            },
        )
        assert not failed, bounded
        bounded_store = bounded["store_path"]
        assert bounded["scan"]["directories_enumerated"] == 3
        assert bounded["scan"]["depth_limited_directories"] == 1
        assert bounded["scan"]["complete_for_policy"] is True
        assert "exhaustive index" in bounded["scan"]["note"]

        # The bound held: nothing from depth 2 was read.
        deep_hit, failed = session.tool(
            "chutni_search",
            {"store_path": bounded_store, "query": "tardigrade cryptobiosis"},
        )
        assert not failed and deep_hit["count"] == 0

        children, failed = session.tool(
            "chutni_children",
            {"store_path": bounded_store, "source_path": str(bounded_source / "Papers")},
        )
        assert not failed, children
        by_name = {
            pathlib.Path(child["display_path"]).name: child
            for child in children["children"]
        }
        assert by_name["Drafts"]["source_kind"] == "directory"
        assert by_name["Drafts"]["observation"] == "opaque"
        assert by_name["paper.txt"]["source_kind"] == "file"
        assert by_name["paper.txt"]["depth"] == 2

        coverage, failed = session.tool(
            "chutni_coverage", {"store_path": bounded_store}
        )
        assert not failed, coverage
        assert coverage["coverage_manifest"]["policy"]["max_depth"] == 1
        assert coverage["coverage_manifest"]["policy"]["memory_goal"] == "define"
        assert (
            coverage["coverage_manifest"]["coverage"][
                "deepest_directory_enumerated"
            ]
            == 1
        )
        assert coverage["coverage_manifest"]["complete_for_policy"] is True
        assert "does not mean the subtree was read" in coverage["interpretation"]

        refused, failed = session.tool(
            "chutni_observe_directory",
            {
                "store_path": bounded_store,
                "source_path": str(bounded_source / "Papers" / "Drafts"),
                "confirmed": False,
            },
        )
        assert failed and refused["error"] == "confirmation_required"

        observed, failed = session.tool(
            "chutni_observe_directory",
            {
                "store_path": bounded_store,
                "source_path": str(bounded_source / "Papers" / "Drafts"),
                "confirmed": True,
                "app_name": "test-host",
                "app_version": "1.0",
            },
        )
        assert not failed, observed
        assert observed["scan"]["directories_enumerated"] == 1
        assert observed["scan"]["files_seen"] == 1

        now_found, failed = session.tool(
            "chutni_search",
            {"store_path": bounded_store, "query": "tardigrade cryptobiosis"},
        )
        assert not failed and now_found["count"] == 1
        assert now_found["results"][0]["depth"] == 3

        # Photos sat at the same depth as Papers and was equally unopened by
        # the observe call; opening one directory must not open its neighbours.
        photos, failed = session.tool(
            "chutni_children",
            {"store_path": bounded_store, "source_path": str(bounded_source / "Photos")},
        )
        assert not failed and photos["count"] == 0

        session.close()

        one_shot_status = one_shot(
            "chutni_folder_status", {"path": str(source)}, env
        )
        assert one_shot_status["action"] == "open_store"
        assert one_shot_status["store_path"] == str(store)

    # Counted from the source rather than carried as a literal, which had
    # drifted to 44 while main() actually ran considerably more.
    print(f"Chutni MCP checks: {assertion_count()} passed, 0 failed")


if __name__ == "__main__":
    main()
