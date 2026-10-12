// Prints every static numeric constant of dev.skycraft.link.Proto as "NAME value".
public final class ProtoDump {
	public static void main(String[] args) throws Exception {
		for (var f : dev.skycraft.link.Proto.class.getFields()) {
			Object v = f.get(null);
			if (v instanceof Number n) System.out.println(f.getName() + " " + (v instanceof Double d ? (long) (double) d : n.longValue()));
		}
	}
}
