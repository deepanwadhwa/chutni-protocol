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
        assert activated["counts"]["artifacts_active"] == 2
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
        assert info["counts"]["sources"] == 1
        assert info["counts"]["artifacts_active"] == 2

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

        session.close()

        one_shot_status = one_shot(
            "chutni_folder_status", {"path": str(source)}, env
        )
        assert one_shot_status["action"] == "open_store"
        assert one_shot_status["store_path"] == str(store)

    print("Chutni MCP checks: 44 passed, 0 failed")


if __name__ == "__main__":
    main()
