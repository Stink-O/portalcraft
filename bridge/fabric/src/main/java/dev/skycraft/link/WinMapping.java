package dev.skycraft.link;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;

import dev.skycraft.SkyCraft;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.foreign.SymbolLookup;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.nio.charset.StandardCharsets;

/**
 * The Windows half of the shared-memory link (Portalcraft Windows port): the host plugin (a 32-bit DLL inside Portal 2) creates a named
 * file mapping, {@code Local\Portalcraft_v1}; this opens it with OpenFileMappingW / MapViewOfFile through the JDK's foreign-function API
 * (the same calls SkyCraft uses). Loaded only when {@link Proto#WINDOWS}; the Linux link never touches this class.
 * Needs {@code --enable-native-access=ALL-UNNAMED} on the JVM command line (the launcher's Windows setup passes it).
 */
final class WinMapping {
	private static final int FILE_MAP_ALL_ACCESS = 0xF001F;

	private static final MethodHandle OPEN_FILE_MAPPING;
	private static final MethodHandle MAP_VIEW_OF_FILE;
	private static final MethodHandle VIRTUAL_QUERY;
	private static final MethodHandle CREATE_MUTEX;
	// OpenFileMappingW's GetLastError, captured right after the call (the JVM may change it later).
	private static final StructLayout CALL_STATE = Linker.Option.captureStateLayout();
	private static final VarHandle LAST_ERROR = CALL_STATE.varHandle(MemoryLayout.PathElement.groupElement("GetLastError"));
	private static final MemorySegment OPEN_STATE = Arena.global().allocate(CALL_STATE);
	private static int lastOpenError = -1;
	private static MemorySegment runningMutex;

	static {
		Linker linker = Linker.nativeLinker();
		SymbolLookup k32 = SymbolLookup.libraryLookup("kernel32", Arena.global());
		OPEN_FILE_MAPPING = linker.downcallHandle(
			k32.find("OpenFileMappingW").orElseThrow(), FunctionDescriptor.of(ADDRESS, JAVA_INT, JAVA_INT, ADDRESS), Linker.Option.captureCallState("GetLastError"));
		MAP_VIEW_OF_FILE = linker.downcallHandle(
			k32.find("MapViewOfFile").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, JAVA_INT, JAVA_INT, JAVA_LONG));
		VIRTUAL_QUERY = linker.downcallHandle(k32.find("VirtualQuery").orElseThrow(), FunctionDescriptor.of(JAVA_LONG, ADDRESS, ADDRESS, JAVA_LONG));
		CREATE_MUTEX = linker.downcallHandle(k32.find("CreateMutexW").orElseThrow(), FunctionDescriptor.of(ADDRESS, ADDRESS, JAVA_INT, ADDRESS));
	}

	private WinMapping() {
	}

	/**
	 * Opens the named mapping and maps all of it. Returns the mapped view sized to the mapping (rounded up to a page), or null when it does
	 * not exist yet / is not accessible (the reason is logged once per Windows error code). The handle and the view stay open for the life
	 * of the process: the host reuses the same object when it restarts while we still hold it.
	 */
	static MemorySegment open(String name) {
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment wname = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
			MemorySegment handle = (MemorySegment) OPEN_FILE_MAPPING.invokeExact(OPEN_STATE, FILE_MAP_ALL_ACCESS, 0, wname);
			if (handle.address() == 0) {
				int error = (int) LAST_ERROR.get(OPEN_STATE, 0L);
				if (error != lastOpenError) {
					lastOpenError = error;
					SkyCraft.LOG.info("SkyCraft: can't open the host's shared memory yet (Windows error {}{})", error,
						error == 2 ? ": the host hasn't created it yet" : error == 5 ? ": access denied; is Portal 2 running as administrator?" : "");
				}
				return null;
			}
			MemorySegment view = (MemorySegment) MAP_VIEW_OF_FILE.invokeExact(handle, FILE_MAP_ALL_ACCESS, 0, 0, 0L);
			if (view.address() == 0) {
				SkyCraft.LOG.error("SkyCraft: MapViewOfFile failed");
				return null;
			}
			// MEMORY_BASIC_INFORMATION (64-bit): RegionSize is at offset 24 and, for a view just mapped, is the mapping's size rounded to pages.
			MemorySegment mbi = arena.allocate(48, 8);
			long got = (long) VIRTUAL_QUERY.invokeExact(view, mbi, 48L);
			long size = got == 0 ? 0 : mbi.get(JAVA_LONG, 24);
			if (size <= 0) {
				SkyCraft.LOG.error("SkyCraft: VirtualQuery of the host mapping failed");
				return null;
			}
			return view.reinterpret(size);
		} catch (Throwable t) {
			SkyCraft.LOG.error("SkyCraft: failed to open shared memory", t);
			return null;
		}
	}

	/** Holds the named mutex {@code name} as long as this Minecraft runs: "a Minecraft is already running" for the host and the launcher. */
	static synchronized void announceRunning(String name) {
		if (runningMutex != null) {
			return;
		}
		try (Arena arena = Arena.ofConfined()) {
			MemorySegment wname = arena.allocateFrom(name, StandardCharsets.UTF_16LE);
			runningMutex = (MemorySegment) CREATE_MUTEX.invokeExact(MemorySegment.NULL, 0, wname);
		} catch (Throwable t) {
			SkyCraft.LOG.warn("SkyCraft: couldn't create the running-Minecraft mutex", t);
		}
	}
}
