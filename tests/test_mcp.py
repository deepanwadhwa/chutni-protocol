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
            "chutni_store_info",
            "chutni_scan",
            "chutni_search",
            "chutni_put_model_artifact",
        }
        assert names == expected

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
        assert activated["counts"]["artifacts_active"] == 1
        assert (store / "manifest.json").is_file()
        assert (store / "catalog.sqlite").is_file()

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
        assert info["counts"]["artifacts_active"] == 1

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

    print("Chutni MCP checks: 26 passed, 0 failed")


if __name__ == "__main__":
    main()
