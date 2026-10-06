# Official qualification results

This directory is reserved for **promoted reference results** from the official Raspberry Pi camera qualification campaign.

Do not place in-progress runs here. Working output belongs under `qualification/work/`.

## Promotion command

Do not copy a completed run into this directory manually. Use the guarded
promotion command:

```bash
hscam-qualify promote \
  qualification/work/<camera> \
  --results-root qualification/results
```

The promoter validates the publication gate, creates a provenance-preserving
destination, copies the canonical result artifacts and derived visual evidence,
omits large raw `.hscap` sample bundles, and records that omission in
`published.json`.

## Promotion layout

The command creates a path such as:

```text
qualification/results/
    <platform>/
        <sensor>/
            <campaign-id>/
                campaign.json
                plan.json
                environment.json
                environment_end.json
                camera.json
                crop_geometry.json
                campaign_status.json
                results.json
                results.csv
                report.md
                samples.json
                published.json
                samples/
                    ...
```

The exact platform key should distinguish materially different Raspberry Pi capture pipelines when necessary. The sensor key should use the sensor model reported by the campaign.

## Promotion gate

A run may be copied from `qualification/work/` into this directory only when:

- `campaign_status.json` reports `"valid": true`;
- the source revision in `plan.json` is a clean committed revision, not `-dirty`;
- the run used the intended committed campaign manifest;
- start/end environment snapshots are present;
- the camera inventory and result aggregates are present;
- `recovery_failure.json` is absent;
- visual sampling is present when enabled, and no visual sample records a generation/export error.

Do not delete rejected, unsupported or unstable cases. They are part of the measured capability boundary.

Do not manually edit numeric test results during promotion. If a run is wrong, fix the implementation/protocol and run a new campaign.

## Large artifacts

The complete timing/configuration dataset and representative visual evidence should be kept with the promoted result.

Large raw HSCAP sequences do not have to be committed when doing so would make the repository impractical. If a raw sequence is omitted, keep:

- its manifest/configuration information;
- timing/result JSON;
- representative PNG/DNG frames where available;
- the generated slow-motion verification video when reasonably sized;
- a note identifying any intentionally omitted large artifact.

## Result interpretation

The checked-in data is not a timeless specification of a camera sensor.

Each result applies to the recorded board, OS, kernel, libcamera version, hscam revision and campaign manifest.

See [Qualification result format](../../docs/qualification-results-v1.md) for the v1 artifact contract.
