import importlib.util
import pathlib
import sys
import tempfile

sys.dont_write_bytecode = True
source = pathlib.Path(__file__).resolve().parents[1] / "scripts" / "generate-paper-graphs.py"
spec = importlib.util.spec_from_file_location("graphs", source)
graphs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(graphs)

with tempfile.TemporaryDirectory() as directory:
    out = pathlib.Path(directory)
    for rows in ([], [{"scenario": "direct", "smoke_duration_ms": "10"}],
                 [{"scenario": "direct", "transition_ms": "", "packet_loss": ""}],
                 [{"scenario": "direct", "transition_ms": "nan", "packet_loss": "0"}],
                 [{"scenario": "direct", "transition_ms": "10", "packet_loss": "0", "status": "fail"}],
                 [{"scenario": "direct", "transition_ms": "10", "packet_loss": "0", "measurement_kind": "runtime_smoke"}]):
        try:
            graphs.generate_metric_graphs(rows, out)
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid metrics accepted: {rows}")
    graphs.generate_metric_graphs([
        dict(scenario="direct", strategy="immediate", transition_ms="10", packet_loss="0"),
        dict(scenario="direct", strategy="graceful", transition_ms="20", packet_loss="1"),
    ], out)
    summary = (out / "metrics-summary.csv").read_text()
    assert "direct / immediate,1,10.000" in summary, summary
    assert "direct / graceful,1,20.000" in summary, summary
print("paper metric validation passed")
