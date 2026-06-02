"""Platform for the Daikin AC."""
import asyncio
from datetime import timedelta
import logging

from pymadoka import Controller, discover_devices, force_device_disconnect
import voluptuous as vol

from homeassistant.config_entries import ConfigEntry
from homeassistant.const import (
    CONF_DEVICE,
    CONF_DEVICES,
    CONF_FORCE_UPDATE,
    CONF_SCAN_INTERVAL,
)
import homeassistant.helpers.config_validation as cv
from homeassistant.core import HomeAssistant

from . import config_flow  # noqa: F401
from .const import CONTROLLERS, DOMAIN

PARALLEL_UPDATES = 0
MIN_TIME_BETWEEN_UPDATES = timedelta(seconds=60)

COMPONENT_TYPES = ["climate", "sensor"]

_LOGGER = logging.getLogger(__name__)

CONFIG_SCHEMA = vol.Schema(
    vol.All(
        cv.deprecated(DOMAIN),
        {
            DOMAIN: vol.Schema(
                {
                    vol.Required(CONF_DEVICES, default=[]): vol.All(
                        cv.ensure_list, [cv.string]
                    ),
                    vol.Optional(CONF_FORCE_UPDATE, default=True): bool,
                    vol.Optional(CONF_DEVICE, default="hci0"): cv.string,
                    vol.Optional(CONF_SCAN_INTERVAL, default=5): cv.positive_int,
                }
            )
        },
    ),
    extra=vol.ALLOW_EXTRA,
)


async def async_setup(hass, config):
    """Set up the component."""

    hass.data.setdefault(DOMAIN, {})

    return True


async def async_setup_entry(hass: HomeAssistant, entry: ConfigEntry):
    """Pass conf to all the components."""

    controllers = {}
    for device in entry.data[CONF_DEVICES]:
        if entry.data[CONF_FORCE_UPDATE]:
            await force_device_disconnect(device)
        controllers[device] = Controller(device, adapter=entry.data[CONF_DEVICE])

    await discover_devices(
        adapter=entry.data[CONF_DEVICE], timeout=entry.data[CONF_SCAN_INTERVAL]
    )

    for device, controller in controllers.items():
        try:
            await asyncio.wait_for(controller.start(), timeout=10)
        except ConnectionAbortedError as connection_aborted_error:
            _LOGGER.error(
                "Could not connect to device %s: %s",
                device,
                str(connection_aborted_error),
            )

    hass.data.setdefault(DOMAIN, {})
    hass.data[DOMAIN][entry.entry_id] = {CONTROLLERS: controllers}
    # Await the platform forwarding so the entry is only reported set up once
    # the climate/sensor platforms actually exist; this also lets platform
    # setup failures propagate (retry the entry) instead of being swallowed in
    # detached tasks, and avoids racing async_unload_entry on a reload.
    await hass.config_entries.async_forward_entry_setups(entry, COMPONENT_TYPES)

    return True


async def async_unload_entry(hass, config_entry):
    """Unload a config entry."""
    unload_ok = await hass.config_entries.async_unload_platforms(
        config_entry, COMPONENT_TYPES
    )

    if unload_ok:
        data = hass.data[DOMAIN].pop(config_entry.entry_id, None)
        if data:
            for device, controller in data[CONTROLLERS].items():
                # Release the BLE connection so a reload can reconnect cleanly.
                # Without this, the old connection lingers and the integration
                # gets stuck "not available" until Home Assistant restarts.
                try:
                    await controller.stop()
                except Exception:  # pylint: disable=broad-except
                    _LOGGER.exception(
                        "Error while disconnecting from device %s", device
                    )

    return unload_ok
