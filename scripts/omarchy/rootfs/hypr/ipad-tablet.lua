-- linux-a9: tablet layer for Omarchy on the iPad (docs/omarchy/tablet-usage.md).
-- Required after ipad.lua from ~/.config/hypr/hyprland.lua. The gestures run
-- outside Hyprland (ipad-gestures, a user unit); the bar buttons are the
-- linuxa9.tablet shell plugin. Both go through ipad-tablet.

local paths = require("default.hypr.paths")

-- ipad-tablet layout/browser keep their choices next to ipad-rotate's.
local function read_state()
  local values = {}
  local file = io.open(paths.state_home .. "/linux-a9/display", "r")
  if not file then
    return values
  end
  for line in file:lines() do
    local key, value = line:match("^%s*([%w_]+)%s*=%s*(%S+)")
    if key then
      values[key] = value
    end
  end
  file:close()
  return values
end

local state = read_state()

-- Layout (ipad-tablet layout): "tablet" shows one window at a time at full
-- size, like iPadOS (Hyprland's monocle; three fingers up/down or Super + Alt
-- + U cycle the windows). "floating" is Omarchy's own floating
-- workspaces, set by ipad-tablet through omarchy-hyprland-workspace-layout-default.
-- "tiling" (the default) leaves Omarchy's tiling alone.
if state.layout == "tablet" then
  hl.config({ general = { layout = "monocle" } })
end

-- Browsers open floating and centred (ipad-tablet browser float), sized for
-- the 1024x768 landscape desktop; Omarchy tiles them otherwise.
if state.browser == "float" then
  o.window({ tag = "chromium-based-browser" }, { float = true, center = true, size = { 940, 660 } })
  o.window({ tag = "firefox-based-browser" }, { float = true, center = true, size = { 940, 660 } })
end

-- Fingers instead of a mouse: windows resize from a wider band around their
-- borders and gaps.
hl.config({
  general = {
    resize_on_border = true,
    extend_border_grab_area = 24,
  },
})

o.bind("SUPER + ALT + O", "On-screen keyboard", "ipad-osk toggle")
-- Rotation lock for ipad-autorotate (the plain SUPER + ALT + R rotates by hand,
-- in ipad.lua).
o.bind("SUPER + ALT + SHIFT + R", "Rotation lock on/off", "ipad-rotate lock toggle")
o.bind("SUPER + ALT + Y", "Tablet layout: tiling, tablet, floating", "ipad-tablet layout next")
o.bind("SUPER + ALT + U", "Next window on this workspace", "ipad-tablet cycle next")
