import pathlib, tempfile, unittest
from chutni import ChutniError, Store

class BindingTest(unittest.TestCase):
    def test_store_lifecycle(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp) / "files"; root.mkdir()
            (root / "note.md").write_text("condensation force from PEG\n")
            (root / "deep").mkdir(); (root / "deep" / "hidden.md").write_text("unopened\n")
            with Store.create(pathlib.Path(tmp) / "Memory.chutni", "files") as store:
                store.add_root(root, max_depth=0, memory_goal="define")
                scan = store.scan(); self.assertTrue(scan["scan"]["complete_for_policy"])
                self.assertTrue(any(x["observation"] == "opaque" for x in store.children(root)))
                hit = store.search("condensation force")[0]
                self.assertEqual(hit["freshness"], "current"); self.assertIn("coverage_manifest_id", hit)
                self.assertEqual(store.coverage()["coverage_manifest"]["coverage"]["depth_limited_directories"], 1)
                source = store.inspect(root / "note.md")["source"]
                result = store.put_artifacts({"producer_kind":"model","name":"test","model_id":"test/model","app_name":"test","app_version":"1"}, "summarize", [{"source_id":source["source_id"], "source_content_hash":source["content_hash"], "text":"PEG note", "artifact_kind":"summary_short", "artifact_origin":"model_generated"}])
                self.assertIn("artifact_id", result["artifacts"][0])
                memory = store.put_memory(
                    "decision",
                    "Use saffron for deployment.",
                    {"producer_kind":"model","name":"test","model_id":"test/model","app_name":"test","app_version":"1"},
                    "record_decision",
                    title="Deployment decision",
                    inputs=[{"message_id":"message-1"}],
                )
                context = store.memory(memory["memory_id"])
                self.assertEqual(context["source"]["freshness"], "current")
                self.assertEqual(context["artifacts"][0]["artifact_kind"], "memory")
                self.assertEqual(store.search("saffron deployment")[0]["source_kind"], "memory")
                with self.assertRaises(ChutniError): store.call("not_real")

if __name__ == "__main__": unittest.main()
