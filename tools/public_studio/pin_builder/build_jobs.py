"""Choose a build parallelism level for the heavy compile stages.

`rebuild` is the dominant cost of every build (230-830s measured), and the
workbench currently pins it to -j2 on every machine. The default here uses most
of the available cores while leaving headroom so the UI and the operator's other
work stay responsive, and it can be overridden.

`WORKBENCH_BUILD_JOBS` wins when set to a positive integer, so a batch run over a
large catalogue can be tuned without editing code.
"""
import os


def resolve_build_jobs(override=None):
    """Return the parallelism to use for cmake/ninja heavy stages."""
    for candidate in (override, os.environ.get("WORKBENCH_BUILD_JOBS")):
        if candidate in (None, ""):
            continue
        try:
            value = int(str(candidate).strip())
        except (TypeError, ValueError):
            continue
        if value > 0:
            return value
    cpus = os.cpu_count() or 4
    # Leave a couple of cores free: these stages also drive ccache and the
    # operator's desktop, and oversubscribing gains little past that.
    return max(2, cpus - 2)
