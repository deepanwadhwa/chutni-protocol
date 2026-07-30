/* Chutni JSON call surface — exercises chutni_call() (T01) end to end.
 *
 * This is not a duplicate of tests/conformance/conformance.c. That suite
 * proves the typed C API against SPEC.md §31 scenarios; this one proves that
 * chutni_call — the JSON-in/JSON-out surface every language binding will
 * actually use — reaches the same store state and, where the two overlap,
 * the same answers. A binding author reading this file sees exactly what
 * every operation's arguments and results look like; that is also why it
 * intentionally prints full raw JSON rather than only pass/fail lines, so a
 * captured run doubles as evidence.
 */
#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE

#include "chutni.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int passes = 0, failures = 0;
static char root_dir[512];

static void ok(const char *scenario, const char *detail) {
    printf("  pass  %-46s %s\n", scenario, detail ? detail : "");
    passes++;
}
static void bad(const char *scenario, const char *detail) {
    printf("  FAIL  %-46s %s\n", scenario, detail ? detail : "");
    failures++;
}
static void check(const char *scenario, int condition, const char *detail) {
    if (condition) ok(scenario, detail);
    else bad(scenario, detail);
}

static void p(char *out, size_t cap, const char *rel) {
    snprintf(out, cap, "%s/%s", root_dir, rel);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

/* A thin wrapper that prints the call (this file doubles as the evidence
   transcript when captured) and fails loudly rather than segfaulting on a
   malformed result if something regresses. */
static char *call(chutni_store *s, const char *op, const char *args) {
    char *result = NULL;
    chutni_status status = chutni_call(s, op, args, &result);
    printf("    chutni_call(%s, %s)\n      -> status=%d %s\n", op,
           args ? args : "{}", status, result ? result : "(null)");
    if (!result) { bad(op, "result_json was NULL"); return NULL; }
    return result;
}

static int contains(const char *haystack, const char *needle) {
    return haystack && needle && strstr(haystack, needle) != NULL;
}

/* Every object this file builds writes "artifact_id" before "artifact_kind"
   (see jcall_artifact_json in src/chutni.c), so the nearest preceding
   artifact_id belongs to the object naming that kind. Good enough for these
   two-artifact fixtures; a real binding parses JSON properly. */
static char *extract_artifact_id_before(const char *json, const char *kind) {
    char kind_needle[64];
    snprintf(kind_needle, sizeof kind_needle, "\"artifact_kind\":\"%s\"", kind);
    const char *kind_at = strstr(json, kind_needle);
    if (!kind_at) return NULL;
    const char *id_needle = "\"artifact_id\":\"";
    const char *id_at = NULL;
    for (const char *p = json; p < kind_at; p++)
        if (!strncmp(p, id_needle, strlen(id_needle))) id_at = p;
    if (!id_at) return NULL;
    id_at += strlen(id_needle);
    const char *end = strchr(id_at, '"');
    if (!end) return NULL;
    size_t len = (size_t)(end - id_at);
    char *out = malloc(len + 1);
    memcpy(out, id_at, len);
    out[len] = 0;
    return out;
}

/* Cheap scalar extraction good enough for this file's own assertions —
   this test does not need a full JSON reader to check "does this string
   appear as a value in the result". */
static char *extract_string(const char *json, const char *key) {
    char needle[128];
    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    const char *at = strstr(json, needle);
    if (!at) return NULL;
    at += strlen(needle);
    const char *end = at;
    while (*end && *end != '"') {
        if (*end == '\\') end++;
        end++;
    }
    size_t len = (size_t)(end - at);
    char *out = malloc(len + 1);
    memcpy(out, at, len);
    out[len] = 0;
    return out;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: call_surface <work-dir>\n"); return 2; }
    snprintf(root_dir, sizeof root_dir, "%s", argv[1]);
    mkdir(root_dir, 0700);
    /* Absolute paths, or every locator comparison below silently compares
       "work/x" against "/home/.../work/x" and finds nothing — the same trap
       the main conformance suite hit once. */
    char abs_root[512];
    if (realpath(root_dir, abs_root)) snprintf(root_dir, sizeof root_dir, "%s", abs_root);

    printf("Chutni JSON call surface — T01 (library %s)\n\n", chutni_library_version());

    char tree[600], note[700], subdir[700], sub_note[700];
    p(tree, sizeof tree, "tree");
    mkdir(tree, 0700);
    p(note, sizeof note, "tree/parb.md");
    write_file(note, "The condensation force was measured at 12 pN using PEG.\n");
    p(subdir, sizeof subdir, "tree/Sub");
    mkdir(subdir, 0700);
    p(sub_note, sizeof sub_note, "tree/Sub/inner.md");
    write_file(sub_note, "Marsupials carry their young in a pouch.\n");

    char store_path[600];
    p(store_path, sizeof store_path, "store.chutni");
    chutni_store *s = NULL;
    if (chutni_create(store_path, "call-surface", &s) != CHUTNI_OK) {
        bad("create store", chutni_last_error(NULL));
        return 1;
    }
    char *r = call(
        s, "put_memory",
        "{\"memory_kind\":\"decision\",\"title\":\"Launch decision\","
        "\"scope\":\"samosa/conversation/demo\","
        "\"text\":\"Use the jalapeño launch plan after legal review.\","
        "\"producer\":{\"producer_kind\":\"model\",\"name\":\"Codex\","
        "\"model_id\":\"gpt-test\",\"model_revision\":\"2026-07-30\","
        "\"app_name\":\"samosa\",\"app_version\":\"0.1\"},"
        "\"operation\":\"record_decision\","
        "\"inputs\":[{\"message_id\":\"message-42\",\"required\":true}]}");
    char *memory_id = extract_string(r, "memory_id");
    check("put_memory stores standalone model work",
          contains(r, "\"memory_kind\":\"decision\"") && memory_id,
          "standalone memory");
    free(r);
    char *manifest = NULL;
    chutni_manifest_json(s, &manifest);
    check("memory use is advertised in the store manifest",
          contains(manifest, "\"standalone_memory\""),
          "capability gating");
    chutni_free(manifest);

    char memory_args[512];
    snprintf(memory_args, sizeof memory_args,
             "{\"source_id\":\"%s\"}", memory_id ? memory_id : "");
    r = call(s, "get_source", memory_args);
    check("standalone memory is not represented as a file",
          contains(r, "\"source_kind\":\"memory\"") &&
          contains(r, "\"display_path\":\"Launch decision\""),
          "memory source kind");
    free(r);

    r = call(s, "source_context", memory_args);
    check("memory context preserves content and provenance",
          contains(r, "jalapeño launch plan") &&
          contains(r, "\"model_id\":\"gpt-test\"") &&
          contains(r, "\"message_id\":\"message-42\"") &&
          contains(r, "\"freshness\":\"current\""),
          "content, model identity, inputs, freshness");
    free(r);

    r = call(s, "search",
             "{\"query\":\"jalapeño launch\",\"limit\":5}");
    check("existing lexical search finds standalone memory",
          contains(r, "\"source_kind\":\"memory\"") &&
          contains(r, "\"freshness\":\"current\"") &&
          contains(r, "\"display_path\":\"Launch decision\""),
          "shared search path");
    free(r);

    r = call(
        s, "put_memory",
        "{\"memory_kind\":\"note\",\"text\":\"must not persist\","
        "\"producer\":{\"producer_kind\":\"model\",\"name\":\"anonymous\"},"
        "\"operation\":\"record_note\"}");
    check("model memory requires host application identity",
          contains(r, "\"error\"") && contains(r, "model producers require"),
          "producer trust");
    free(r);

    char root_id[CHUTNI_ID_STRLEN];
    chutni_root_add(s, tree, "tree", NULL, root_id);

    /* --------------------------------------------------------- store-less */

    r = call(NULL, "capabilities", NULL);
    check("capabilities: reports this spec version",
          contains(r, "\"spec_version\":\"" CHUTNI_SPEC_VERSION "\""), "§30");
    check("capabilities: lists v0.2 artifact kinds",
          contains(r, "coverage_manifest") && contains(r, "source_definition"),
          "§15.5-15.7");
    free(r);

    r = call(NULL, "discover", NULL);
    check("discover: returns without a store handle", contains(r, "\"count\":"), "§39");
    free(r);

    char *bad_op = call(s, "not_a_real_operation", NULL);
    check("unknown operation returns the error envelope, not a crash",
          contains(bad_op, "\"error\":") && contains(bad_op, "\"code\":") &&
          contains(bad_op, "\"message\":"), "§20");
    free(bad_op);

    r = call((chutni_store *)0, "search", "{\"query\":\"x\"}");
    check("a store-requiring operation with no store fails cleanly",
          contains(r, "\"error\":") && contains(r, "requires an open store"), "");
    free(r);

    r = call(s, "scan", "{not json}");
    check("malformed JSON is rejected instead of becoming an empty write request",
          contains(r, "\"error\":") && contains(r, "valid JSON object"), "");
    free(r);

    r = call(s, "store_info", "[]");
    check("a JSON value that is not an object is rejected",
          contains(r, "\"error\":") && contains(r, "must be a JSON object"), "");
    free(r);

    /* -------------------------------------------------------------- scan */

    char scan_args[128];
    snprintf(scan_args, sizeof scan_args, "{\"app_name\":\"call-surface\"}");
    r = call(s, "scan", scan_args);
    check("scan via chutni_call indexes the fixture",
          contains(r, "\"files_seen\":2") && contains(r, "\"complete_for_policy\":true"),
          "§20 scan(root_id, policy)");
    free(r);

    r = call(s, "store_info", NULL);
    check("store_info reports the same counts scan produced",
          contains(r, "\"sources_files\":2"), "");
    free(r);

    /* ------------------------------------------- search, cross-checked
     *
     * The ticket's explicit requirement: drive search through chutni_call
     * only, then confirm it agrees with the typed API on the same store. */

    r = call(s, "search", "{\"query\":\"condensation force\",\"limit\":5}");
    char *via_call_artifact = extract_string(r, "artifact_id");
    check("search via chutni_call finds the fixture text",
          contains(r, "parb.md") && via_call_artifact != NULL, "");
    free(r);

    chutni_search_request typed_request;
    memset(&typed_request, 0, sizeof typed_request);
    typed_request.query = "condensation force";
    typed_request.limit = 5;
    chutni_search_result *typed_results = NULL;
    size_t typed_count = 0;
    chutni_search(s, &typed_request, &typed_results, &typed_count);
    int search_agrees = typed_count == 1 && via_call_artifact &&
                        typed_results[0].artifact_id &&
                        !strcmp(typed_results[0].artifact_id, via_call_artifact);
    check("chutni_call search agrees with the typed API",
          search_agrees, "same store, same query, same artifact_id");
    chutni_search_result_free(typed_results, typed_count);
    free(via_call_artifact);

    /* ----------------------------------------- children, cross-checked */

    char children_args[700];
    snprintf(children_args, sizeof children_args, "{\"source_path\":\"%s\"}", tree);
    r = call(s, "children", children_args);
    check("children via chutni_call lists the immediate entries",
          contains(r, "\"count\":2") && contains(r, "parb.md") && contains(r, "Sub"),
          "§20 list_children(source_id)");
    free(r);

    char root_source_id[CHUTNI_ID_STRLEN];
    chutni_source_find(s, tree, root_source_id);
    chutni_source_info *typed_children = NULL;
    size_t typed_child_count = 0;
    chutni_list_children(s, root_source_id, &typed_children, &typed_child_count);
    check("chutni_call children agrees with the typed API on count",
          typed_child_count == 2, "");
    chutni_source_info_free(typed_children, typed_child_count);

    /* ------------------------------------------- coverage, cross-checked */

    r = call(s, "coverage", NULL);
    char *manifest_id_via_call = extract_string(r, "coverage_manifest_id");
    check("coverage via chutni_call reports completeness honestly",
          contains(r, "\"complete_for_policy\":true") &&
          contains(r, "does not mean the subtree was read"), "§15.7, §35.1 rule 5");
    free(r);

    char *typed_coverage_json = NULL;
    chutni_get_coverage(s, root_id, &typed_coverage_json);
    int coverage_agrees = manifest_id_via_call && typed_coverage_json &&
                          strstr(typed_coverage_json, manifest_id_via_call) != NULL;
    check("chutni_call coverage agrees with the typed API",
          coverage_agrees, "same coverage_manifest_id");
    chutni_free(typed_coverage_json);
    free(manifest_id_via_call);

    /* --------------------------------------- observe_directory, get_*, etc */

    char observe_args[700];
    snprintf(observe_args, sizeof observe_args, "{\"source_path\":\"%s\"}", subdir);
    r = call(s, "observe_directory", observe_args);
    check("observe_directory via chutni_call opens exactly that directory",
          contains(r, "\"directories_enumerated\":1"), "§20 observe_directory");
    free(r);

    r = call(s, "get_source", children_args);
    check("get_source returns the resolved source",
          contains(r, "\"source_kind\":\"directory\""), "§20 get_source(source_id)");
    free(r);

    char list_args[700];
    snprintf(list_args, sizeof list_args, "{\"source_path\":\"%s\"}", note);
    r = call(s, "list_artifacts", list_args);
    char *artifact_id = extract_artifact_id_before(r, "extracted_text");
    check("list_artifacts returns this source's active artifacts",
          contains(r, "extracted_text") && artifact_id != NULL, "§20 list_artifacts");
    free(r);

    if (artifact_id) {
        char get_artifact_args[128];
        snprintf(get_artifact_args, sizeof get_artifact_args,
                 "{\"artifact_id\":\"%s\"}", artifact_id);
        r = call(s, "get_artifact", get_artifact_args);
        char *echoed_id = extract_string(r, "artifact_id");
        check("get_artifact returns the exact artifact requested",
              contains(r, "\"artifact_kind\":\"extracted_text\"") &&
              echoed_id && !strcmp(echoed_id, artifact_id),
              "§20 get_artifact(artifact_id)");
        free(echoed_id);
        free(r);

        char fresh_args[128];
        snprintf(fresh_args, sizeof fresh_args, "{\"artifact_id\":\"%s\"}", artifact_id);
        r = call(s, "check_freshness", fresh_args);
        check("check_freshness reports current for an untouched artifact",
              contains(r, "\"freshness\":\"current\""), "§13.3, §20 check_freshness");
        free(r);
    }
    free(artifact_id);

    r = call(s, "source_context", list_args);
    check("source_context groups every current interpretation",
          contains(r, "\"artifact_count\":") && contains(r, "extracted_text"), "§20");
    free(r);

    /* ----------------------------------------------------- read_object */

    chutni_artifact_info *note_artifacts = NULL;
    size_t note_artifact_count = 0;
    char note_source_id[CHUTNI_ID_STRLEN];
    chutni_source_find(s, note, note_source_id);
    chutni_list_artifacts(s, note_source_id, &note_artifacts, &note_artifact_count);
    char *listing_object_hash = NULL;
    for (size_t i = 0; i < note_artifact_count; i++)
        if (note_artifacts[i].object_hash)
            listing_object_hash = strdup(note_artifacts[i].object_hash);
    chutni_artifact_info_free(note_artifacts, note_artifact_count);
    if (listing_object_hash) {
        char obj_args[160];
        snprintf(obj_args, sizeof obj_args, "{\"object_hash\":\"%s\"}", listing_object_hash);
        r = call(s, "read_object", obj_args);
        check("read_object resolves an object_hash to its payload",
              contains(r, "\"object_hash\":"), "§20 read_object(object_hash), §14");
        free(r);
    } else {
        /* file_metadata is inline in this fixture; a directory_listing
           artifact is always object-backed, so fall through to that. */
        chutni_artifact_info *dir_artifacts = NULL;
        size_t dir_artifact_count = 0;
        chutni_list_artifacts(s, root_source_id, &dir_artifacts, &dir_artifact_count);
        char *dir_object_hash = NULL;
        for (size_t i = 0; i < dir_artifact_count; i++)
            if (dir_artifacts[i].object_hash) dir_object_hash = strdup(dir_artifacts[i].object_hash);
        chutni_artifact_info_free(dir_artifacts, dir_artifact_count);
        if (dir_object_hash) {
            char obj_args[160];
            snprintf(obj_args, sizeof obj_args, "{\"object_hash\":\"%s\"}", dir_object_hash);
            r = call(s, "read_object", obj_args);
            check("read_object resolves an object_hash to its payload",
                  contains(r, "\"object_hash\":") && contains(r, "\"text\":"),
                  "§20 read_object(object_hash), §14");
            free(r);
            free(dir_object_hash);
        } else {
            bad("read_object resolves an object_hash to its payload",
                "fixture produced no object-backed artifact to test against");
        }
    }
    free(listing_object_hash);

    /* -------------------------------------------- put_artifacts, cross-checked
     *
     * chutni_call's put_artifacts is deliberately lower-level than the typed
     * API's convenience wrapper: each artifact carries its own source_id and
     * source_content_hash, mirroring chutni_artifact directly. */

    char sub_note_source_id[CHUTNI_ID_STRLEN];
    chutni_source_find(s, sub_note, sub_note_source_id);
    char sub_note_hash[CHUTNI_HASH_STRLEN];
    chutni_hash_file(sub_note, sub_note_hash);

    char put_args[1024];
    snprintf(put_args, sizeof put_args,
             "{\"operation\":\"summarize\","
             "\"producer\":{\"producer_kind\":\"model\",\"name\":\"call-surface-model\","
             "\"model_id\":\"example/call-surface\",\"app_name\":\"call-surface\","
             "\"app_version\":\"1\"},"
             "\"artifacts\":[{\"source_id\":\"%s\",\"source_content_hash\":\"%s\","
             "\"text\":\"A note about marsupial pouches.\","
             "\"artifact_kind\":\"summary_short\",\"artifact_origin\":\"model_generated\"}]}",
             sub_note_source_id, sub_note_hash);
    r = call(s, "put_artifacts", put_args);
    char *put_artifact_id = extract_string(r, "artifact_id");
    check("put_artifacts via chutni_call writes a model-generated artifact",
          put_artifact_id != NULL && contains(r, "\"semantic_validation\":\"not_performed\""),
          "§16, §20 put_artifacts");
    free(r);

    if (put_artifact_id) {
        chutni_artifact_info *typed_check = NULL;
        size_t typed_check_count = 0;
        chutni_list_artifacts(s, sub_note_source_id, &typed_check, &typed_check_count);
        int found_via_typed = 0;
        for (size_t i = 0; i < typed_check_count; i++)
            if (typed_check[i].artifact_id && !strcmp(typed_check[i].artifact_id, put_artifact_id))
                found_via_typed = 1;
        check("chutni_call put_artifacts agrees with the typed API",
              found_via_typed, "same artifact_id visible via chutni_list_artifacts");
        chutni_artifact_info_free(typed_check, typed_check_count);
    }

    /* A malformed batch (missing source_content_hash) must fail cleanly. */
    r = call(s, "put_artifacts",
             "{\"operation\":\"summarize\","
             "\"producer\":{\"producer_kind\":\"model\",\"name\":\"x\"},"
             "\"artifacts\":[{\"source_id\":\"nope\",\"text\":\"x\","
             "\"artifact_kind\":\"summary_short\",\"artifact_origin\":\"model_generated\"}]}");
    check("put_artifacts refuses an artifact missing source_content_hash",
          contains(r, "\"error\":"), "§13.3");
    free(r);

    /* ---------------------------------------- representation, cross-checked
     *
     * The MCP surface has no representation tool today; this is new. */

    if (put_artifact_id) {
        char rep_args[512];
        snprintf(rep_args, sizeof rep_args,
                 "{\"artifact_id\":\"%s\","
                 "\"profile\":{\"representation_kind\":\"text_embedding\","
                 "\"model_id\":\"example/embed\",\"model_revision\":\"1\","
                 "\"dtype\":\"f32\",\"normalization\":\"none\"},"
                 "\"vector\":[0.10,0.20,0.30,0.40]}",
                 put_artifact_id);
        r = call(s, "put_representation", rep_args);
        char *representation_id = extract_string(r, "representation_id");
        check("put_representation via chutni_call stores a vector",
              representation_id != NULL && contains(r, "\"dimensions\":4"),
              "§17, §20 (new: no MCP tool covers this)");
        free(r);

        if (representation_id) {
            chutni_representation_info *typed_reps = NULL;
            size_t typed_rep_count = 0;
            chutni_representations_list(s, put_artifact_id, &typed_reps, &typed_rep_count);
            int rep_agrees = 0;
            for (size_t i = 0; i < typed_rep_count; i++)
                if (typed_reps[i].representation_id &&
                    !strcmp(typed_reps[i].representation_id, representation_id))
                    rep_agrees = 1;
            check("chutni_call put_representation agrees with the typed API",
                  rep_agrees, "same representation_id visible via chutni_representations_list");
            chutni_representation_info_free(typed_reps, typed_rep_count);
        }

        char search_sem_args[512];
        snprintf(search_sem_args, sizeof search_sem_args,
                 "{\"vector\":[0.11,0.19,0.31,0.39],"
                 "\"profile\":{\"representation_kind\":\"text_embedding\","
                 "\"model_id\":\"example/embed\",\"model_revision\":\"1\","
                 "\"dtype\":\"f32\",\"normalization\":\"none\"},\"limit\":5}");
        r = call(s, "search_semantic", search_sem_args);
        check("search_semantic via chutni_call finds the stored vector",
              contains(r, "\"count\":1") && contains(r, "cosine_bruteforce"),
              "§17.5, §19.1, §20 (new: no MCP tool covers this)");
        free(r);

        r = call(s, "put_representation",
                 "{\"artifact_id\":\"not-used\",\"profile\":{},\"vector\":[\"not-a-number\"]}");
        check("embedding vectors reject non-numeric JSON values",
              contains(r, "\"error\":") && contains(r, "numbers only"), "§17 vector input");
        free(r);

        free(representation_id);
    }
    free(put_artifact_id);

    /* -------------------------------------------------- mutation operations */

    char extra_dir[700], extra_file[700];
    p(extra_dir, sizeof extra_dir, "tree/Extra");
    mkdir(extra_dir, 0700);
    p(extra_file, sizeof extra_file, "tree/Extra/e.txt");
    write_file(extra_file, "an extra file added after the first scan\n");

    char add_args[700];
    snprintf(add_args, sizeof add_args, "{\"root_id\":\"%s\",\"path\":\"%s\"}",
             root_id, extra_file);
    r = call(s, "add_source", add_args);
    check("add_source via chutni_call records a new file source",
          contains(r, "\"changed\":true"), "§20 add_or_update_source(locator)");
    free(r);

    char missing_args[700];
    snprintf(missing_args, sizeof missing_args, "{\"source_path\":\"%s\"}", extra_file);
    r = call(s, "mark_source_missing", missing_args);
    check("mark_source_missing via chutni_call sets the missing state",
          contains(r, "\"state\":\"missing\""), "§20 mark_source_missing(source_id)");
    free(r);

    r = call(s, "forget_source", missing_args);
    check("forget_source via chutni_call removes the source",
          contains(r, "\"forgotten\":true"), "§20, §24.3 forget_source(source_id, mode)");
    free(r);

    r = call(s, "forget_source", memory_args);
    check("standalone memory uses the ordinary forget path",
          contains(r, "\"forgotten\":true"), "§20, §24.3");
    free(r);
    r = call(s, "search", "{\"query\":\"jalapeño launch\"}");
    check("forgotten standalone memory is no longer searchable",
          contains(r, "\"count\":0"), "shared index cleanup");
    free(r);

    r = call(s, "rebuild_indexes", NULL);
    check("rebuild_indexes via chutni_call succeeds", contains(r, "\"ok\":true"),
          "§20 rebuild_indexes()");
    free(r);
    free(memory_id);

    /* -------------------------------------------------- read-only refusal */

    chutni_close(s);
    chutni_store *ro = NULL;
    if (chutni_open(store_path, 1, &ro) == CHUTNI_OK) {
        r = call(ro, "scan", "{}");
        check("a mutating operation on a read-only store is refused",
              contains(r, "\"error\":"), "§20, matches typed CHUTNI_ERR_READONLY");
        free(r);
        chutni_close(ro);
    } else {
        bad("a mutating operation on a read-only store is refused", "could not reopen read-only");
    }

    printf("\n%d passed, %d failed\n", passes, failures);
    return failures ? 1 : 0;
}
