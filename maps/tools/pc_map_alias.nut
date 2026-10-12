// portalcraft: pc_pit_flings and pc_testbed are built under new file names. Valve's scripts choose
// GLaDOS's lines, the elevator videos and music, the portal gun and the next map from GetMapName(),
// so report the original name to them. Run by the first logic_script in the map, before any of
// Valve's scripts.
if (!("__pc_real_GetMapName" in getroottable()))
{
	::__pc_real_GetMapName <- ::GetMapName
	::__pc_map_alias <- { pc_pit_flings = "sp_a2_pit_flings", pc_testbed = "sp_a2_pit_flings" }
	::GetMapName <- function()
	{
		local n = ::__pc_real_GetMapName()
		return (n in ::__pc_map_alias) ? ::__pc_map_alias[n] : n
	}
}
printl("[portalcraft] GetMapName() -> " + GetMapName())
