--[[
Control script for the raw-tcp-udp-output plugin.

The URL in OBS Settings -> Stream never reaches plugin outputs: the
frontend always uses its own RTMP/SRT outputs there. This script starts
the raw output directly, with its own low-latency x264 encoder, so it
runs independently of (and in parallel with) normal streaming/recording.

Install: OBS -> Tools -> Scripts -> "+" -> select this file.
Set the destination URL, then use the Start/Stop buttons.
]]

obs = obslua

local output = nil
local encoder = nil

local url = "udp://127.0.0.1:9999"
local bitrate = 6000
local resolution_mode = "auto" -- auto | base | custom
local custom_width = 1280
local custom_height = 720

local function stop_stream()
	if output ~= nil then
		obs.obs_output_stop(output)
		obs.obs_output_release(output)
		output = nil
	end
	if encoder ~= nil then
		obs.obs_encoder_release(encoder)
		encoder = nil
	end
end

local function start_stream()
	stop_stream()

	local enc_settings = obs.obs_data_create()
	obs.obs_data_set_string(enc_settings, "rate_control", "CBR")
	obs.obs_data_set_int(enc_settings, "bitrate", bitrate)
	obs.obs_data_set_int(enc_settings, "keyint_sec", 1)
	obs.obs_data_set_string(enc_settings, "preset", "veryfast")
	obs.obs_data_set_string(enc_settings, "tune", "zerolatency")
	-- repeat SPS/PPS on every keyframe; raw streams carry no container
	-- extradata, so the decoder needs them inline
	obs.obs_data_set_string(enc_settings, "x264opts", "repeat-headers=1")
	encoder = obs.obs_video_encoder_create("obs_x264", "raw-output-x264",
					       enc_settings, nil)
	obs.obs_data_release(enc_settings)

	-- Resolution: the encoder follows the current OBS output (scaled)
	-- resolution by default; "base" rescales to the canvas size and
	-- "custom" to a user-defined size. Queried at every start, so
	-- changes in Settings -> Video are picked up automatically.
	local ovi = obs.obs_video_info()
	obs.obs_get_video_info(ovi)
	local w, h = 0, 0
	if resolution_mode == "base" then
		w, h = ovi.base_width, ovi.base_height
	elseif resolution_mode == "custom" then
		w, h = custom_width, custom_height
	end
	w, h = w - (w % 2), h - (h % 2) -- x264 needs even dimensions
	if w > 0 and h > 0 and
	   (w ~= ovi.output_width or h ~= ovi.output_height) then
		obs.obs_encoder_set_scaled_size(encoder, w, h)
	end

	obs.obs_encoder_set_video(encoder, obs.obs_get_video())

	local out_settings = obs.obs_data_create()
	obs.obs_data_set_string(out_settings, "url", url)
	output = obs.obs_output_create("raw_video_tcp_udp_output",
				       "raw-tcp-udp-output", out_settings, nil)
	obs.obs_data_release(out_settings)

	if output == nil then
		print("ERROR: output 'raw_video_tcp_udp_output' not found - " ..
		      "is the plugin DLL installed?")
		stop_stream()
		return
	end

	obs.obs_output_set_video_encoder(output, encoder)

	if obs.obs_output_start(output) then
		print(string.format("raw output started: %s at %dx%d", url,
				    obs.obs_encoder_get_width(encoder),
				    obs.obs_encoder_get_height(encoder)))
	else
		local err = obs.obs_output_get_last_error(output)
		print("ERROR: failed to start raw output: " .. (err or "unknown"))
		stop_stream()
	end
end

----------------------------------------------------------------------
-- OBS script API
----------------------------------------------------------------------

function script_description()
	return "Starts the Raw Video TCP/UDP Output with a low-latency " ..
	       "x264 encoder.\nPair it with tools/raw_video_server.py on " ..
	       "the receiving end."
end

function script_properties()
	local props = obs.obs_properties_create()
	obs.obs_properties_add_text(props, "url",
		"Destination (tcp:// or udp://host:port)", obs.OBS_TEXT_DEFAULT)
	obs.obs_properties_add_int(props, "bitrate", "Bitrate (kbps)",
				   500, 50000, 500)

	local res = obs.obs_properties_add_list(props, "resolution",
		"Resolution", obs.OBS_COMBO_TYPE_LIST,
		obs.OBS_COMBO_FORMAT_STRING)
	obs.obs_property_list_add_string(res, "Auto (match OBS output)", "auto")
	obs.obs_property_list_add_string(res, "Match OBS canvas (base)", "base")
	obs.obs_property_list_add_string(res, "Custom", "custom")
	obs.obs_properties_add_int(props, "width", "Custom width", 2, 7680, 2)
	obs.obs_properties_add_int(props, "height", "Custom height", 2, 4320, 2)
	obs.obs_property_set_modified_callback(res,
		function(properties, property, settings)
			local custom =
				obs.obs_data_get_string(settings, "resolution")
				== "custom"
			obs.obs_property_set_visible(
				obs.obs_properties_get(properties, "width"),
				custom)
			obs.obs_property_set_visible(
				obs.obs_properties_get(properties, "height"),
				custom)
			return true
		end)

	obs.obs_properties_add_button(props, "start", "Start raw output",
		function() start_stream() return false end)
	obs.obs_properties_add_button(props, "stop", "Stop raw output",
		function() stop_stream() return false end)
	return props
end

function script_defaults(settings)
	obs.obs_data_set_default_string(settings, "url", "udp://127.0.0.1:9999")
	obs.obs_data_set_default_int(settings, "bitrate", 6000)
	obs.obs_data_set_default_string(settings, "resolution", "auto")
	obs.obs_data_set_default_int(settings, "width", 1280)
	obs.obs_data_set_default_int(settings, "height", 720)
end

function script_update(settings)
	url = obs.obs_data_get_string(settings, "url")
	bitrate = obs.obs_data_get_int(settings, "bitrate")
	resolution_mode = obs.obs_data_get_string(settings, "resolution")
	custom_width = obs.obs_data_get_int(settings, "width")
	custom_height = obs.obs_data_get_int(settings, "height")
end

function script_unload()
	stop_stream()
end
