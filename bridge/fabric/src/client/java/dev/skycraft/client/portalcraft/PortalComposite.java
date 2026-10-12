package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import org.joml.Matrix4f;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL13;
import org.lwjgl.opengl.GL20;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL33;
import org.lwjgl.system.MemoryStack;

/**
 * Portalcraft: draws a portal's opening into a target (PortalPasses). The opening is Portal 2's
 * ellipse at its current open scale, in the portal's plane, generated in the vertex shader (no
 * vertex buffer): {@link #SEGMENTS} triangles fanned from the centre, depth-tested against the
 * target's depth (reversed-Z, so terrain in front of the portal hides it) and writing the plane's
 * depth. Inside it the fragment is either the view through the portal (the pass's own target at
 * the same pixel, colour and depth-in-alpha alike) or, past the recursion limit, the portal's flat
 * colour with the plane's own depth encoded in alpha. A full-screen variant covers the camera that
 * sits in the opening itself.
 *
 * <p>Plain GL on Minecraft's render thread into our own framebuffer over the target's textures;
 * every piece of GL state touched is saved and restored (as {@link DepthAlphaPass}).
 */
public final class PortalComposite {
	public static final int SEGMENTS = 64;
	private static int program, vao, fbo;
	private static int uMvp, uViewRot, uCentre, uAxisR, uAxisU, uSegs, uFull, uFlat, uColour, uScale, uSrc;
	private static boolean failed;
	private static final int[] VP = new int[4];
	private static final boolean[] CM = new boolean[4];
	private static final Matrix4f MVP = new Matrix4f();
	private static int fboFailLog;

	private PortalComposite() {
	}

	private static final String VS = """
		#version 330 core
		uniform mat4 mvp;
		uniform mat4 viewRot;
		uniform vec3 centre;
		uniform vec3 axisR;
		uniform vec3 axisU;
		uniform int segs;
		uniform int full;
		out float vDepth;
		void main() {
			if (full != 0) {
				vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
				gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
				vDepth = 0.0;
				return;
			}
			int tri = gl_VertexID / 3;
			int k = gl_VertexID - tri * 3;
			vec3 p = centre;
			if (k > 0) {
				float t = 6.28318530718 * float(tri + k - 1) / float(segs);
				p += axisR * cos(t) + axisU * sin(t);
			}
			gl_Position = mvp * vec4(p, 1.0);
			vDepth = -(viewRot * vec4(p, 1.0)).z;
		}
		""";
	private static final String FS = """
		#version 330 core
		uniform sampler2D src;
		uniform int flatFill;
		uniform vec3 colour;
		uniform vec2 scale;
		in float vDepth;
		out vec4 outColor;
		void main() {
			if (flatFill != 0) {
				float a = 0.0;
				if (vDepth > 0.0) {
					float t = clamp(log(vDepth / %DMIN%) / log(%DMAX% / %DMIN%), 0.0, 1.0);
					a = floor(255.0 - t * 254.0) / 255.0;
				}
				outColor = vec4(colour, a);
				return;
			}
			outColor = texelFetch(src, ivec2(gl_FragCoord.xy * scale), 0);
		}
		""".replace("%DMIN%", Float.toString(DepthAlphaPass.D_MIN)).replace("%DMAX%", Float.toString(DepthAlphaPass.D_MAX));

	private static int compile(int type, String src) {
		int s = GL20.glCreateShader(type);
		GL20.glShaderSource(s, src);
		GL20.glCompileShader(s);
		if (GL20.glGetShaderi(s, GL20.GL_COMPILE_STATUS) == 0) {
			SkyCraft.LOG.error("Portalcraft: portal composite shader: {}", GL20.glGetShaderInfoLog(s));
			return 0;
		}
		return s;
	}

	private static boolean init() {
		int vs = compile(GL20.GL_VERTEX_SHADER, VS), fs = compile(GL20.GL_FRAGMENT_SHADER, FS);
		if (vs == 0 || fs == 0) {
			return false;
		}
		program = GL20.glCreateProgram();
		GL20.glAttachShader(program, vs);
		GL20.glAttachShader(program, fs);
		GL20.glLinkProgram(program);
		GL20.glDeleteShader(vs);
		GL20.glDeleteShader(fs);
		if (GL20.glGetProgrami(program, GL20.GL_LINK_STATUS) == 0) {
			SkyCraft.LOG.error("Portalcraft: portal composite link: {}", GL20.glGetProgramInfoLog(program));
			return false;
		}
		uMvp = GL20.glGetUniformLocation(program, "mvp");
		uViewRot = GL20.glGetUniformLocation(program, "viewRot");
		uCentre = GL20.glGetUniformLocation(program, "centre");
		uAxisR = GL20.glGetUniformLocation(program, "axisR");
		uAxisU = GL20.glGetUniformLocation(program, "axisU");
		uSegs = GL20.glGetUniformLocation(program, "segs");
		uFull = GL20.glGetUniformLocation(program, "full");
		uFlat = GL20.glGetUniformLocation(program, "flatFill");
		uColour = GL20.glGetUniformLocation(program, "colour");
		uScale = GL20.glGetUniformLocation(program, "scale");
		uSrc = GL20.glGetUniformLocation(program, "src");
		vao = GL30.glGenVertexArrays();
		fbo = GL30.glGenFramebuffers();
		return true;
	}

	/**
	 * Draw one opening into the target (dstColor + dstDepth, w x h).
	 *
	 * @param srcColor  the pass's colour texture (ignored when {@code flatColour >= 0})
	 * @param srcScale  source pixels per target pixel (the pass's resolution scale)
	 * @param proj      the target camera's projection (crop and oblique plane included)
	 * @param viewRot   its view rotation
	 * @param centre    the opening's centre relative to that camera (blocks)
	 * @param axisR     half-width axis (blocks), axisU half-height axis
	 * @param full      the camera sits in the opening: cover the whole target, no depth test
	 * @param flatColour 0xRRGGBB to fill with the portal's colour (recursion limit), or -1
	 */
	public static boolean draw(int dstColor, int dstDepth, int w, int h, int srcColor, float srcScale, Matrix4f proj, Matrix4f viewRot, float[] centre, float[] axisR,
		float[] axisU, boolean full, int flatColour) {
		if (failed) {
			return false;
		}
		if (program == 0 && !init()) {
			failed = true;
			return false;
		}
		int prevProgram = GL11.glGetInteger(GL20.GL_CURRENT_PROGRAM);
		int prevVao = GL11.glGetInteger(GL30.GL_VERTEX_ARRAY_BINDING);
		int prevDraw = GL11.glGetInteger(GL30.GL_DRAW_FRAMEBUFFER_BINDING);
		int prevRead = GL11.glGetInteger(GL30.GL_READ_FRAMEBUFFER_BINDING);
		int prevActive = GL11.glGetInteger(GL13.GL_ACTIVE_TEXTURE);
		final int[] vp = VP;
		GL11.glGetIntegerv(GL11.GL_VIEWPORT, vp);
		final boolean[] cm = CM;
		try (MemoryStack st = MemoryStack.stackPush()) {
			var b = st.malloc(4);
			GL11.glGetBooleanv(GL11.GL_COLOR_WRITEMASK, b);
			for (int i = 0; i < 4; i++) {
				cm[i] = b.get(i) != 0;
			}
		}
		boolean depthTest = GL11.glIsEnabled(GL11.GL_DEPTH_TEST), blend = GL11.glIsEnabled(GL11.GL_BLEND), cull = GL11.glIsEnabled(GL11.GL_CULL_FACE),
			scissor = GL11.glIsEnabled(GL11.GL_SCISSOR_TEST), stencil = GL11.glIsEnabled(GL11.GL_STENCIL_TEST), polyOffset = GL11.glIsEnabled(GL11.GL_POLYGON_OFFSET_FILL);
		boolean depthMask = GL11.glGetBoolean(GL11.GL_DEPTH_WRITEMASK);
		int depthFunc = GL11.glGetInteger(GL11.GL_DEPTH_FUNC);
		GL13.glActiveTexture(GL13.GL_TEXTURE0);
		int prevTex = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
		int prevSampler = GL11.glGetInteger(GL33.GL_SAMPLER_BINDING);
		try {
			GL30.glBindFramebuffer(GL30.GL_FRAMEBUFFER, fbo);
			// attached every call: Minecraft may recreate a target and GL may reuse a texture id
			GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, dstColor, 0);
			GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_DEPTH_ATTACHMENT, GL11.GL_TEXTURE_2D, dstDepth, 0);
			if (GL30.glCheckFramebufferStatus(GL30.GL_FRAMEBUFFER) != GL30.GL_FRAMEBUFFER_COMPLETE) {
				if (fboFailLog++ < 3) {
					SkyCraft.LOG.error("Portalcraft: portal composite framebuffer incomplete");
				}
				return false;
			}
			GL11.glViewport(0, 0, w, h);
			GL11.glColorMask(true, true, true, true);
			GL11.glDisable(GL11.GL_BLEND);
			GL11.glDisable(GL11.GL_CULL_FACE);
			GL11.glDisable(GL11.GL_SCISSOR_TEST);
			GL11.glDisable(GL11.GL_STENCIL_TEST);
			GL11.glDisable(GL11.GL_POLYGON_OFFSET_FILL);
			if (full) {
				GL11.glDisable(GL11.GL_DEPTH_TEST);
				GL11.glDepthMask(false);
			} else {
				GL11.glEnable(GL11.GL_DEPTH_TEST);
				GL11.glDepthFunc(GL11.GL_GEQUAL);  // reversed-Z: nearer is greater
				GL11.glDepthMask(true);
			}
			GL20.glUseProgram(program);
			GL30.glBindVertexArray(vao);
			GL33.glBindSampler(0, 0);
			GL11.glBindTexture(GL11.GL_TEXTURE_2D, flatColour >= 0 ? 0 : srcColor);
			try (MemoryStack st = MemoryStack.stackPush()) {
				var m = st.mallocFloat(16);
				MVP.set(proj).mul(viewRot).get(m);
				GL20.glUniformMatrix4fv(uMvp, false, m);
				viewRot.get(m);
				GL20.glUniformMatrix4fv(uViewRot, false, m);
			}
			GL20.glUniform3f(uCentre, centre[0], centre[1], centre[2]);
			GL20.glUniform3f(uAxisR, axisR[0], axisR[1], axisR[2]);
			GL20.glUniform3f(uAxisU, axisU[0], axisU[1], axisU[2]);
			GL20.glUniform1i(uSegs, SEGMENTS);
			GL20.glUniform1i(uFull, full ? 1 : 0);
			GL20.glUniform1i(uFlat, flatColour >= 0 ? 1 : 0);
			GL20.glUniform3f(uColour, ((flatColour >> 16) & 255) / 255.0F, ((flatColour >> 8) & 255) / 255.0F, (flatColour & 255) / 255.0F);
			GL20.glUniform2f(uScale, srcScale, srcScale);
			GL20.glUniform1i(uSrc, 0);
			GL11.glDrawArrays(GL11.GL_TRIANGLES, 0, full ? 3 : SEGMENTS * 3);
			return true;
		} finally {
			GL11.glBindTexture(GL11.GL_TEXTURE_2D, prevTex);
			GL33.glBindSampler(0, prevSampler);
			GL13.glActiveTexture(prevActive);
			GL30.glBindVertexArray(prevVao);
			GL20.glUseProgram(prevProgram);
			GL30.glBindFramebuffer(GL30.GL_DRAW_FRAMEBUFFER, prevDraw);
			GL30.glBindFramebuffer(GL30.GL_READ_FRAMEBUFFER, prevRead);
			GL11.glViewport(vp[0], vp[1], vp[2], vp[3]);
			GL11.glColorMask(cm[0], cm[1], cm[2], cm[3]);
			if (depthTest) GL11.glEnable(GL11.GL_DEPTH_TEST); else GL11.glDisable(GL11.GL_DEPTH_TEST);
			GL11.glDepthMask(depthMask);
			GL11.glDepthFunc(depthFunc);
			if (blend) GL11.glEnable(GL11.GL_BLEND); else GL11.glDisable(GL11.GL_BLEND);
			if (cull) GL11.glEnable(GL11.GL_CULL_FACE); else GL11.glDisable(GL11.GL_CULL_FACE);
			if (scissor) GL11.glEnable(GL11.GL_SCISSOR_TEST); else GL11.glDisable(GL11.GL_SCISSOR_TEST);
			if (stencil) GL11.glEnable(GL11.GL_STENCIL_TEST); else GL11.glDisable(GL11.GL_STENCIL_TEST);
			if (polyOffset) GL11.glEnable(GL11.GL_POLYGON_OFFSET_FILL); else GL11.glDisable(GL11.GL_POLYGON_OFFSET_FILL);
		}
	}
}
