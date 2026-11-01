"""Printer drivers. To add a printer family, write drivers/<name>.py (see drivers/base.py and
CONTRIBUTING.md) and add its class to DRIVERS below."""
from drivers.base import PrinterError
from drivers.halot import HalotDriver

DRIVERS = {d.id: d for d in (HalotDriver,)}
DEFAULT = HalotDriver.id


def get(driver_id):
    """The driver class for an id from settings.json."""
    try:
        return DRIVERS[driver_id or DEFAULT]
    except KeyError:
        raise PrinterError(f"Unknown printer type: {driver_id}", "unknown_driver", driver=driver_id) from None
