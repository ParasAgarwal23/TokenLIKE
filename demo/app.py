"""TokenLIKE demo: compare TokenLIKE with the fused-decode and raw baselines.

    streamlit run demo/app.py                                  # live mode
    streamlit run demo/app.py -- --replay results/final.json   # saved results

Live mode runs the CLI for each mode and (if available) Person B's DuckDB
checker. A query is shown as verified ONLY when the checker passed for those
exact row ids. Replay mode shows saved measurements and says so.
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace

import altair as alt
import pandas as pd
import streamlit as st

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts" / "bench"))
import run_bench as rb  # noqa: E402

MODES = rb.MODES
MODE_LABEL = {"token": "TokenLIKE (token IDs)", "decode": "Fused decode + match",
              "raw": "Raw strings (uncompressed)"}
BAR_COLOR = "#2a78d6"


def cli_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument("--replay", type=Path, default=None)
    ap.add_argument("--patterns", type=Path, default=ROOT / "patterns/core.txt")
    ap.add_argument("--col", default="data/clickbench/url")
    ap.add_argument("--vocab", default="data/vocab.bin")
    ap.add_argument("--build", type=Path, default=ROOT / "build")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--checker", default=None,
                    help="checker command template (same as run_bench.py --checker)")
    return ap.parse_args(sys.argv[1:])


ARGS = cli_args()


# ---------------------------------------------------------------- data access

@st.cache_data
def load_replay(path: str) -> dict:
    return json.loads(Path(path).read_text(encoding="utf-8"))


def live_run(pattern: str, runs: int) -> dict:
    cli = rb.find_cli(ARGS.build)
    opts = SimpleNamespace(col=ARGS.col, col2=None, vocab=ARGS.vocab, runs=runs, warmup=1,
                           pin_cpu=0, timeout=300)
    tmp = Path(tempfile.mkdtemp(prefix="tokenlike_demo_"))
    res = {"pattern": pattern, "modes": {}}
    for mode in MODES:
        res["modes"][mode] = rb.run_cli(cli, opts, mode, pattern, tmp / f"{mode}.bin")
    ok = {m: r for m, r in res["modes"].items() if r["status"] == "ok"}
    res["agree"] = (len({r["matches"] for r in ok.values()}) <= 1 and
                    len({r["rowids_sha256"] for r in ok.values()}) <= 1)
    checker = ARGS.checker
    if checker is None and (ROOT / rb.DEFAULT_CHECKER).exists():
        checker = (f'"{sys.executable}" {rb.DEFAULT_CHECKER} '
                   "--col {col} --pattern {pattern} --rowids {rowids}")
    target = "token" if "token" in ok else (sorted(ok)[0] if ok else None)
    if not res["agree"]:
        res["verified"], res["check"] = "fail", {"detail": "modes disagree"}
    elif target and checker:
        res["check"] = rb.run_checker(checker, opts, pattern, tmp / f"{target}.bin")
        res["check"]["mode_checked"] = target
        res["verified"] = res["check"]["verified"]
    else:
        res["verified"] = "not_run"
    return res


# ---------------------------------------------------------------- rendering

def badge(res: dict) -> None:
    v = res.get("verified")
    checked = res.get("check", {}).get("mode_checked")
    if v == "pass":
        st.success(f"✅ Verified against DuckDB: row ids from **{checked}** mode match exactly.")
    elif v in ("fail", "error"):
        st.error(f"❌ Verification FAILED: {res.get('check', {}).get('detail', '')}")
    else:
        st.info("⚪ Not verified: DuckDB checker did not run for this result.")
    if not res.get("agree", True):
        st.error("❌ The modes returned different results.")


def show_result(res: dict) -> None:
    modes = res["modes"]
    ok = {m: r for m, r in modes.items() if r.get("status") == "ok"}

    badge(res)

    cols = st.columns(len(MODES))
    for c, m in zip(cols, MODES):
        r = modes.get(m, {})
        if r.get("status") == "ok":
            c.metric(MODE_LABEL[m], f"{r['matches']:,} matches", f"{r['ns_per_row']:.2f} ns/row",
                     delta_color="off")
        else:
            c.metric(MODE_LABEL[m], "unavailable", r.get("status", "missing"), delta_color="off")
            if r.get("stderr"):
                c.caption(r["stderr"][-300:])

    if not ok:
        return

    rows = [{"mode": MODE_LABEL[m], "ns/row": r["ns_per_row"]} for m, r in ok.items()]
    df = pd.DataFrame(rows)
    st.subheader("Scan time (median, ns per row; lower is faster)")
    bars = alt.Chart(df).mark_bar(color=BAR_COLOR, cornerRadiusEnd=4, height=22).encode(
        x=alt.X("ns/row:Q", title="ns per row"),
        y=alt.Y("mode:N", title=None, sort=[MODE_LABEL[m] for m in MODES]),
        tooltip=[alt.Tooltip("mode:N"), alt.Tooltip("ns/row:Q", format=".2f")],
    )
    labels = bars.mark_text(align="left", dx=4).encode(text=alt.Text("ns/row:Q", format=".2f"))
    st.altair_chart((bars + labels).properties(height=150), width="stretch")

    if "token" in ok and "decode" in ok and ok["token"]["ns_per_row"] > 0:
        ratio = ok["decode"]["ns_per_row"] / ok["token"]["ns_per_row"]
        word = "faster" if ratio >= 1 else "slower"
        st.markdown(f"TokenLIKE is **{max(ratio, 1 / ratio):.2f}× {word}** than fused decode "
                    "on this pattern (measured).")

    def kb(x):
        return "-" if x is None else f"{x:,.1f}"

    table = pd.DataFrame([{
        "mode": MODE_LABEL[m],
        "matches": r["matches"],
        "ns/row": round(r["ns_per_row"], 3),
        "GB/s (original text)": round(r["gbps"], 3),
        "tokens read/row": round(r["tokens_per_row"], 2),
        "bytes decoded/row": round(r["bytes_decoded_per_row"], 2),
        "matcher table KB": kb(r.get("table_kb")),
        "dense table KB": kb(r.get("dense_table_kb")),
        "build µs": round(r["build_us"], 1),
        "runs": r["runs"],
    } for m, r in ok.items()])
    st.subheader("Work done and memory")
    st.dataframe(table, hide_index=True, width="stretch")
    st.caption("Raw mode scans uncompressed strings (a different, larger representation); it "
               "shows the speed if no compression were used, not a like-for-like competitor.")


# ---------------------------------------------------------------- page

st.set_page_config(page_title="TokenLIKE", layout="wide")
st.title("TokenLIKE: exact LIKE on compressed token IDs")

replay_default = ARGS.replay is not None
mode = st.sidebar.radio("Mode", ["Live", "Replay"], index=1 if replay_default else 0)

if mode == "Replay":
    path = st.sidebar.text_input("Results file", str(ARGS.replay or "results/final.json"))
    try:
        data = load_replay(path)
    except (OSError, json.JSONDecodeError) as e:
        st.error(f"Cannot load replay file: {e}")
        st.stop()
    meta = data.get("meta", {})
    st.warning(
        f"**REPLAY: saved measurements, not a fresh benchmark.** "
        f"Commit `{str(meta.get('git_commit'))[:10]}`"
        f"{' (dirty tree)' if meta.get('git_dirty') else ''} · {meta.get('cpu')} · "
        f"{meta.get('timestamp_utc')} · {meta.get('runs')} runs, median")
    results = data.get("results", [])
    if not results:
        st.error("Replay file has no results.")
        st.stop()
    labels = [r["pattern"] for r in results]
    pick = st.sidebar.selectbox("LIKE pattern", range(len(labels)), format_func=lambda i: labels[i])
    st.code(f"WHERE {meta.get('col', 'col')} LIKE '{labels[pick]}'", language="sql")
    show_result(results[pick])
else:
    try:
        patterns = rb.read_patterns(ARGS.patterns)
    except OSError:
        patterns = []
    if not patterns:
        st.sidebar.warning(f"No patterns file at {ARGS.patterns}")
    pattern = st.sidebar.selectbox("LIKE pattern", patterns) if patterns else None
    custom = st.sidebar.text_input("…or a custom pattern", "")
    pattern = custom or pattern
    runs = st.sidebar.number_input("Runs (median)", 1, 50, ARGS.runs)
    st.caption(f"Live · column `{ARGS.col}`")
    if pattern:
        st.code(f"WHERE url LIKE '{pattern}'", language="sql")
    if pattern and st.sidebar.button("Run", type="primary"):
        with st.spinner("Running token, decode and raw modes…"):
            try:
                st.session_state["live"] = live_run(pattern, int(runs))
            except SystemExit as e:  # find_cli exits when the binary is missing
                st.error(f"Live run failed: {e}. Use Replay mode.")
            except Exception as e:  # keep the demo alive
                st.error(f"Live run failed: {e}. Use Replay mode.")
    res = st.session_state.get("live")
    if res and res["pattern"] == pattern:
        show_result(res)
