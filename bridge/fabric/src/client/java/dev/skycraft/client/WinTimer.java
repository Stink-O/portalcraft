package dev.skycraft.client;

import dev.skycraft.SkyCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;

import static java.lang.foreign.ValueLayout.JAVA_INT;

/**
 * Windows only: ask for a 1 ms system timer for this process (winmm timeBeginPeriod(1), what most games do). Windows otherwise rounds
 * every sleep up to its default tick of about 15.6 ms, so SkyClient.paceFrame's 0.25 ms waits for the host's next frame slept 15.6 ms
 * each and the hidden Minecraft settled at ~25 fps with 5 ms of work a frame (2026-10-12): the host reprojected 40 ms old frames, which
 * showed as the stretched band at the screen edge when turning and smeared block edges when moving. Since Windows 10 2004 the setting
 * only affects the process that asks; it ends with the process.
 */
public final class WinTimer {
	private static boolean done;

	private WinTimer() {
	}

	public static void ensure() {
		if (done) {
			return;
		}
		done = true;
		if (!System.getProperty("os.name", "").toLowerCase(java.util.Locale.ROOT).startsWith("windows")) {
			return;
		}
		// Windows 11 throttles a process whose windows are all hidden (Minecraft's is): "efficiency mode" scheduling, and it ignores the timer
		// request below. The process opts out of both (PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1 | IGNORE_TIMER_RESOLUTION 0x4 in the control
		// mask, none in the state mask: never throttled). Older Windows does not know the request and refuses it: harmless.
		try {
			Linker linker = Linker.nativeLinker();
			SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
			MethodHandle setInfo = linker.downcallHandle(k32.find("SetProcessInformation").orElseThrow(),
				FunctionDescriptor.of(JAVA_INT, java.lang.foreign.ValueLayout.ADDRESS, JAVA_INT, java.lang.foreign.ValueLayout.ADDRESS, JAVA_INT));
			MethodHandle self = linker.downcallHandle(k32.find("GetCurrentProcess").orElseThrow(), FunctionDescriptor.of(java.lang.foreign.ValueLayout.ADDRESS));
			java.lang.foreign.MemorySegment state = Arena.global().allocate(12);
			state.set(JAVA_INT, 0, 1);         // Version (PROCESS_POWER_THROTTLING_CURRENT_VERSION)
			state.set(JAVA_INT, 4, 0x1 | 0x4); // ControlMask
			state.set(JAVA_INT, 8, 0);         // StateMask: off for both
			int ok = (int) setInfo.invokeExact((java.lang.foreign.MemorySegment) self.invokeExact(), 4 /* ProcessPowerThrottling */, state, 12);
			SkyCraft.LOG.info("Portalcraft: Windows power throttling for the hidden Minecraft turned off ({})", ok != 0 ? "ok" : "refused by this Windows");
		} catch (Throwable t) {
			SkyCraft.LOG.warn("Portalcraft: could not turn off Windows power throttling ({})", t.toString());
		}
		try {
			SymbolLookup winmm = SymbolLookup.libraryLookup("winmm", Arena.global());
			MethodHandle begin = Linker.nativeLinker().downcallHandle(winmm.find("timeBeginPeriod").orElseThrow(), FunctionDescriptor.of(JAVA_INT, JAVA_INT));
			int r = (int) begin.invokeExact(1);
			SkyCraft.LOG.info("Portalcraft: Windows timer resolution 1 ms requested (timeBeginPeriod: {})", r == 0 ? "ok" : "error " + r);
		} catch (Throwable t) {
			SkyCraft.LOG.warn("Portalcraft: could not request a 1 ms Windows timer ({}); Minecraft's frames may be paced coarsely", t.toString());
		}
	}
}
