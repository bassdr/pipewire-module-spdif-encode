-- spdif-encode-lock-volume.lua
--
-- Dynamically discovers which device the spdif-encode module outputs to
-- (from target.object on the spdif-encode-output node) and forces that
-- device's route volume to 1.0 (100%).  Any volume scaling corrupts the
-- IEC 61937 encoded bitstream.
--
-- No manual configuration needed — the target device is read from the
-- running PipeWire graph automatically.

cutils = require ("common-utils")
log = Log.open_topic ("s-spdif-encode")

locked_device_id = nil

-- Resolve the spdif-encode-output node's target into a device bound-id.
function resolve_target_device (nodes_om)
  -- find the encoder output node
  local enc_node = nodes_om:lookup {
    Constraint { "node.name", "=", "spdif-encode-output", type = "pw" },
  }
  if not enc_node then
    return nil
  end

  local target_name = enc_node.properties ["target.object"]
  if not target_name then
    return nil
  end

  -- find the target node by name
  local target_node = nodes_om:lookup {
    Constraint { "node.name", "=", target_name, type = "pw" },
  }
  if not target_node then
    return nil
  end

  -- return its parent device id
  return target_node.properties ["device.id"]
end

-- Force all active routes on a device to volume 1.0 / unmuted.
function force_unit_volume (device)
  for p in device:iterate_params ("Route") do
    local route = cutils.parseParam (p, "Route")
    if not route or not route.props or not route.props.properties then
      goto next_route
    end

    local props = route.props.properties
    local volumes = props.channelVolumes
    local needs_reset = false

    if volumes then
      for _, v in ipairs (volumes) do
        if math.abs (v - 1.0) > 1e-6 then
          needs_reset = true
          break
        end
      end
    end

    if props.mute then
      needs_reset = true
    end

    if needs_reset then
      log:warning ("spdif-encode: resetting volume on route " ..
          route.name .. " (encoded bitstream requires 100%)")

      local n_channels = volumes and #volumes or 2
      local unit_volumes = {}
      for i = 1, n_channels do
        unit_volumes [i] = 1.0
      end
      table.insert (unit_volumes, 1, "Spa:Float")

      local param = Pod.Object {
        "Spa:Pod:Object:Param:Route", "Route",
        index = route.index,
        device = route.device,
        props = Pod.Object {
          "Spa:Pod:Object:Param:Props", "Route",
          mute = false,
          channelVolumes = Pod.Array (unit_volumes),
        },
        save = false,
      }

      device:set_param ("Route", param)
    end

    ::next_route::
  end
end

function try_lock_device (source)
  local nodes_om = source:call ("get-object-manager", "node")
  if not nodes_om then
    return
  end

  local dev_id = resolve_target_device (nodes_om)
  if not dev_id then
    return
  end

  locked_device_id = dev_id
  log:warning ("spdif-encode: locking volume on device id " ..
      tostring (dev_id))

  -- immediately enforce on the device
  local devices_om = source:call ("get-object-manager", "device")
  if devices_om then
    local dev = devices_om:lookup {
      Constraint { "bound-id", "=", dev_id, type = "gobject" },
    }
    if dev then
      force_unit_volume (dev)
    end
  end
end

-- When device route params change, check if it's our locked device.
SimpleEventHook {
  name = "spdif-encode/lock-device-volume",
  after = { "device/store-or-restore-routes" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    local device = event:get_subject ()
    if not locked_device_id then
      return
    end

    if tostring (device ["bound-id"]) ~= tostring (locked_device_id) then
      return
    end

    force_unit_volume (device)
  end
}:register ()

-- Track the spdif-encode-output node to know which device to protect.
-- When the node appears, resolve its target device.
-- When it disappears, release the lock.
SimpleEventHook {
  name = "spdif-encode/track-encoder-node",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-added" },
      Constraint { "media.class", "=", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    local node = event:get_subject ()
    if node.properties ["node.name"] ~= "spdif-encode-output" then
      return
    end

    local source = event:get_source ()
    try_lock_device (source)
  end
}:register ()

SimpleEventHook {
  name = "spdif-encode/untrack-encoder-node",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "node-removed" },
      Constraint { "media.class", "=", "Stream/Output/Audio" },
    },
  },
  execute = function (event)
    local node = event:get_subject ()
    if node.properties ["node.name"] ~= "spdif-encode-output" then
      return
    end

    if locked_device_id then
      log:warning ("spdif-encode: releasing volume lock on device id " ..
          tostring (locked_device_id))
      locked_device_id = nil
    end
  end
}:register ()

-- On any device-params-changed Route event, if we haven't locked yet,
-- try to find the encoder node (it may have appeared before we loaded).
SimpleEventHook {
  name = "spdif-encode/late-discover",
  after = { "device/store-or-restore-routes" },
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "device-params-changed" },
      Constraint { "event.subject.param-id", "=", "Route" },
    },
  },
  execute = function (event)
    if locked_device_id then
      return
    end

    local source = event:get_source ()
    try_lock_device (source)
  end
}:register ()
