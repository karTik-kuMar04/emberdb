#!/usr/bin/env python3
"""Writes deploy/grafana/dashboards/emberdb.json. Run it after changing the panels below."""
import json
import pathlib

DS = {"type": "prometheus", "uid": "prometheus"}
I = '{instance=~"$instance"}'  # every query is filtered by the dashboard's instance variable

panels = []


def add(title, queries, x, y, w, h, unit="short", kind="timeseries", maximum=None, note=""):
    defaults = {"unit": unit}
    if maximum is not None:
        defaults["min"] = 0
        defaults["max"] = maximum
    panels.append({
        "id": len(panels) + 1,
        "type": kind,
        "title": title,
        "description": note,
        "datasource": DS,
        "gridPos": {"x": x, "y": y, "w": w, "h": h},
        "fieldConfig": {"defaults": defaults, "overrides": []},
        "targets": [
            {"datasource": DS, "expr": expr, "legendFormat": legend, "refId": chr(ord("A") + i)}
            for i, (expr, legend) in enumerate(queries)
        ],
    })


# Row 1: at-a-glance numbers
add("Instances up", [('sum(up{job="emberdb"})', "")], 0, 0, 4, 4, kind="stat")
add("Clients", [(f"sum(emberdb_connected_clients{I})", "")], 4, 0, 4, 4, kind="stat")
add("Keys", [(f"sum(emberdb_keys{I})", "")], 8, 0, 4, 4, kind="stat")
add("Memory used", [(f"sum(emberdb_memory_used_bytes{I})", "")], 12, 0, 4, 4, unit="bytes", kind="stat")
add("Uptime", [(f"min(emberdb_uptime_seconds{I})", "")], 16, 0, 4, 4, unit="s", kind="stat")
add("Errors per second", [(f"sum(rate(emberdb_command_errors_total{I}[1m]))", "")], 20, 0, 4, 4,
    unit="ops", kind="stat")

# Row 2: the three numbers that describe a single-threaded server
add("Commands per second",
    [(f"sum by (instance) (rate(emberdb_commands_processed_total{I}[1m]))", "{{instance}}")],
    0, 4, 8, 8, unit="ops")
add("Command latency (server side)",
    [(f"histogram_quantile(0.5, sum by (le, instance) (rate(emberdb_command_duration_seconds_bucket{I}[1m])))",
      "p50 {{instance}}"),
     (f"histogram_quantile(0.99, sum by (le, instance) (rate(emberdb_command_duration_seconds_bucket{I}[1m])))",
      "p99 {{instance}}")],
    8, 4, 8, 8, unit="s", note="Time spent executing a command. Network and queueing time are not included.")
add("Event loop busy",
    [(f"rate(emberdb_event_loop_busy_seconds_total{I}[1m])", "{{instance}}")],
    16, 4, 8, 8, unit="percentunit", maximum=1,
    note="One thread serves every client. Near 100% means every client gets slower.")

# Row 3: memory, keys, clients
add("Memory used vs limit",
    [(f"emberdb_memory_used_bytes{I}", "used {{instance}}"),
     (f"emberdb_memory_limit_bytes{I} > 0", "limit {{instance}}")],
    0, 12, 8, 8, unit="bytes")
add("Expired and evicted keys per second",
    [(f"rate(emberdb_keys_expired_total{I}[1m])", "expired {{instance}}"),
     (f"rate(emberdb_keys_evicted_total{I}[1m])", "evicted {{instance}}")],
    8, 12, 8, 8, unit="ops")
add("Connected clients", [(f"emberdb_connected_clients{I}", "{{instance}}")], 16, 12, 8, 8)

# Row 4: replication and connection drops
add("Replication lag (unsent bytes)",
    [(f"emberdb_replication_follower_max_pending_bytes{I}", "{{instance}}")],
    0, 20, 8, 8, unit="bytes")
add("Replication link",
    [(f"emberdb_replication_link_up{I}", "link up {{instance}}"),
     (f"emberdb_replication_connected_followers{I}", "followers {{instance}}")],
    8, 20, 8, 8)
add("Dropped connections per second",
    [(f"sum by (reason) (rate(emberdb_connections_dropped_total{I}[5m]))", "{{reason}}")],
    16, 20, 8, 8, unit="ops")

# Row 5: traffic, persistence, security
add("Network throughput",
    [(f"sum by (instance) (rate(emberdb_network_read_bytes_total{I}[1m]))", "read {{instance}}"),
     (f"sum by (instance) (rate(emberdb_network_written_bytes_total{I}[1m]))", "written {{instance}}")],
    0, 28, 6, 8, unit="Bps")
add("AOF size", [(f"emberdb_aof_size_bytes{I}", "{{instance}}")], 6, 28, 6, 8, unit="bytes")
add("Busiest commands",
    [(f"topk(5, sum by (command) (rate(emberdb_commands_processed_total{I}[1m])))", "{{command}}")],
    12, 28, 6, 8, unit="ops")
add("Wrong passwords per second",
    [(f"rate(emberdb_auth_failures_total{I}[1m])", "{{instance}}")], 18, 28, 6, 8, unit="ops")

dashboard = {
    "uid": "emberdb",
    "title": "emberdb",
    "tags": ["emberdb"],
    "schemaVersion": 39,
    "version": 1,
    "editable": True,
    "refresh": "10s",
    "time": {"from": "now-15m", "to": "now"},
    "annotations": {"list": []},
    "templating": {"list": [{
        "name": "instance",
        "label": "Instance",
        "type": "query",
        "datasource": DS,
        "query": {"query": "label_values(emberdb_build_info, instance)", "refId": "instances"},
        "refresh": 2,
        "sort": 1,
        "includeAll": True,
        "multi": True,
        "allValue": ".*",
        "current": {"selected": True, "text": ["All"], "value": ["$__all"]},
    }]},
    "panels": panels,
}

out = pathlib.Path(__file__).parent / "dashboards" / "emberdb.json"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(dashboard, indent=2) + "\n")
print(f"wrote {out} ({len(panels)} panels)")
