package dev.skycraft.client.portalcraft;

import dev.skycraft.SkyCraft;
import org.joml.Matrix4f;
import org.lwjgl.opengl.GL11;
import org.lwjgl.opengl.GL13;
import org.lwjgl.opengl.GL14;
import org.lwjgl.opengl.GL20;
import org.lwjgl.opengl.GL30;
import org.lwjgl.opengl.GL33;
import org.lwjgl.system.MemoryStack;

/**
 * Portalcraft: writes every pixel's depth into the alpha channel of Minecraft's level colour
 * (before it is read back for the host), on the GPU, at full resolution. Alpha = log-encoded
 * linear depth along the view axis, unprojected with the frame's own projection (oblique near
 * plane of portal views included): 255 at {@link #D_MIN} blocks, 1 at {@link #D_MAX}, 0 where
 * nothing was drawn (sky). The host decodes it per pixel to hide Portal 2 objects behind
 * Minecraft terrain, so colour and depth share one image and one mapping (no second readback,
 * no resampling: edges coincide pixel for pixel).
 *
 * <p>Plain GL on Minecraft's render thread, into our own framebuffer over Minecraft's colour
 * texture; every piece of GL state touched is saved and restored.
 */
public final class DepthAlphaPass {
	public static final float D_MIN = 0.25F, D_MAX = 512.0F;  // blocks (protocol: kDepthAlphaMin/Max)
	private static int program, vao, fbo, fboTex = -1, uInvProj, uSize, uZ01, uFar, uDepth;
	private static boolean failed, logged, formatLogged;

	private DepthAlphaPass() {
	}

	private static final String VS = """
		#version 330 core
		void main() {
			vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
			gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
		}
		""";
	private static final String FS = """
		#version 330 core
		uniform sampler2D depthTex;
		uniform mat4 invProj;
		uniform vec2 size;
		uniform int z01;
		uniform float farD;
		out vec4 outColor;
		void main() {
			vec2 uv = gl_FragCoord.xy / size;
			float w = texture(depthTex, uv).r;
			float nz = z01 != 0 ? w : 2.0 * w - 1.0;
			vec4 v = invProj * vec4(uv * 2.0 - 1.0, nz, 1.0);
			float d = abs(v.w) > 1e-12 ? -v.z / v.w : 0.0;
			float a = 0.0;
			if (d > 0.0 && d < farD * 0.98) {
				float t = clamp(log(d / %DMIN%) / log(%DMAX% / %DMIN%), 0.0, 1.0);
				// rounded towards "farther": the decoded depth is never nearer than the true one, so a
				// Portal 2 object lying on this surface (a portal on terrain) is never hidden by it
				a = floor(255.0 - t * 254.0) / 255.0;
			}
			outColor = vec4(0.0, 0.0, 0.0, a);
		}
		""".replace("%DMIN%", Float.toString(D_MIN)).replace("%DMAX%", Float.toString(D_MAX));

	private static int compile(int type, String src) {
		int s = GL20.glCreateShader(type);
		GL20.glShaderSource(s, src);
		GL20.glCompileShader(s);
		if (GL20.glGetShaderi(s, GL20.GL_COMPILE_STATUS) == 0) {
			SkyCraft.LOG.error("Portalcraft: depth-alpha shader: {}", GL20.glGetShaderInfoLog(s));
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
			SkyCraft.LOG.error("Portalcraft: depth-alpha link: {}", GL20.glGetProgramInfoLog(program));
			return false;
		}
		uInvProj = GL20.glGetUniformLocation(program, "invProj");
		uSize = GL20.glGetUniformLocation(program, "size");
		uZ01 = GL20.glGetUniformLocation(program, "z01");
		uFar = GL20.glGetUniformLocation(program, "farD");
		uDepth = GL20.glGetUniformLocation(program, "depthTex");
		vao = GL30.glGenVertexArrays();
		fbo = GL30.glGenFramebuffers();
		return true;
	}

	/** Alpha := encoded depth, for the whole colour texture. False if it could not run. */
	public static boolean run(int colorTex, int depthTex, int width, int height, Matrix4f proj, boolean zZeroToOne, float far) {
		return run(colorTex, depthTex, width, height, width, height, proj, zZeroToOne, far);
	}

	/** As above, for the bottom-left subW x subH pixels only (the shipped crop, v13). */
	public static boolean run(int colorTex, int depthTex, int width, int height, int subW, int subH, Matrix4f proj, boolean zZeroToOne, float far) {
		if (failed) {
			return false;
		}
		if (program == 0 && !init()) {
			failed = true;
			return false;
		}
		if (!formatLogged) {
			formatLogged = true;
			int prev = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
			GL11.glBindTexture(GL11.GL_TEXTURE_2D, colorTex);
			int fmt = GL11.glGetTexLevelParameteri(GL11.GL_TEXTURE_2D, 0, GL11.GL_TEXTURE_INTERNAL_FORMAT);
			int abits = GL11.glGetTexLevelParameteri(GL11.GL_TEXTURE_2D, 0, GL30.GL_TEXTURE_ALPHA_SIZE);
			GL11.glBindTexture(GL11.GL_TEXTURE_2D, prev);
			SkyCraft.LOG.info("Portalcraft: level colour texture internal format 0x{}, alpha bits {}", Integer.toHexString(fmt), abits);
		}
		// save
		int prevProgram = GL11.glGetInteger(GL20.GL_CURRENT_PROGRAM);
		int prevVao = GL11.glGetInteger(GL30.GL_VERTEX_ARRAY_BINDING);
		int prevDraw = GL11.glGetInteger(GL30.GL_DRAW_FRAMEBUFFER_BINDING);
		int prevRead = GL11.glGetInteger(GL30.GL_READ_FRAMEBUFFER_BINDING);
		int prevActive = GL11.glGetInteger(GL13.GL_ACTIVE_TEXTURE);
		int[] vp = new int[4];
		GL11.glGetIntegerv(GL11.GL_VIEWPORT, vp);
		boolean[] cm = new boolean[4];
		try (MemoryStack st = MemoryStack.stackPush()) {
			var b = st.malloc(4);
			GL11.glGetBooleanv(GL11.GL_COLOR_WRITEMASK, b);
			for (int i = 0; i < 4; i++) {
				cm[i] = b.get(i) != 0;
			}
		}
		int[] sc = new int[4];
		GL11.glGetIntegerv(GL11.GL_SCISSOR_BOX, sc);
		boolean depthTest = GL11.glIsEnabled(GL11.GL_DEPTH_TEST), blend = GL11.glIsEnabled(GL11.GL_BLEND), cull = GL11.glIsEnabled(GL11.GL_CULL_FACE),
			scissor = GL11.glIsEnabled(GL11.GL_SCISSOR_TEST), stencil = GL11.glIsEnabled(GL11.GL_STENCIL_TEST);
		boolean depthMask = GL11.glGetBoolean(GL11.GL_DEPTH_WRITEMASK);
		GL13.glActiveTexture(GL13.GL_TEXTURE0);
		int prevTex = GL11.glGetInteger(GL11.GL_TEXTURE_BINDING_2D);
		int prevSampler = GL11.glGetInteger(GL33.GL_SAMPLER_BINDING);
		int prevCompare = 0;
		try {
			// our framebuffer over Minecraft's colour texture
			GL30.glBindFramebuffer(GL30.GL_FRAMEBUFFER, fbo);
			// attached every frame: Minecraft recreates its render target when the frame size changes,
			// and GL may hand the new texture the same id (a cached attachment then wrote nowhere)
			GL30.glFramebufferTexture2D(GL30.GL_FRAMEBUFFER, GL30.GL_COLOR_ATTACHMENT0, GL11.GL_TEXTURE_2D, colorTex, 0);
			fboTex = colorTex;
			if (GL30.glCheckFramebufferStatus(GL30.GL_FRAMEBUFFER) != GL30.GL_FRAMEBUFFER_COMPLETE) {
				if (!logged) {
					logged = true;
					SkyCraft.LOG.error("Portalcraft: depth-alpha framebuffer incomplete");
				}
				return false;
			}
			GL11.glViewport(0, 0, width, height);
			GL11.glColorMask(false, false, false, true);
			GL11.glDisable(GL11.GL_DEPTH_TEST);
			GL11.glDepthMask(false);
			GL11.glDisable(GL11.GL_BLEND);
			GL11.glDisable(GL11.GL_CULL_FACE);
			if (subW < width || subH < height) {
				GL11.glEnable(GL11.GL_SCISSOR_TEST);
				GL11.glScissor(0, 0, subW, subH);
			} else {
				GL11.glDisable(GL11.GL_SCISSOR_TEST);
			}
			GL11.glDisable(GL11.GL_STENCIL_TEST);
			GL20.glUseProgram(program);
			GL30.glBindVertexArray(vao);
			GL33.glBindSampler(0, 0);
			GL11.glBindTexture(GL11.GL_TEXTURE_2D, depthTex);
			prevCompare = GL11.glGetTexParameteri(GL11.GL_TEXTURE_2D, GL14.GL_TEXTURE_COMPARE_MODE);
			if (prevCompare != GL11.GL_NONE) {
				GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL14.GL_TEXTURE_COMPARE_MODE, GL11.GL_NONE);
			}
			Matrix4f inv = new Matrix4f(proj).invert();
			try (MemoryStack st = MemoryStack.stackPush()) {
				var m = st.mallocFloat(16);
				inv.get(m);
				GL20.glUniformMatrix4fv(uInvProj, false, m);
			}
			GL20.glUniform2f(uSize, width, height);
			GL20.glUniform1i(uZ01, zZeroToOne ? 1 : 0);
			GL20.glUniform1f(uFar, far);
			GL20.glUniform1i(uDepth, 0);
			GL11.glDrawArrays(GL11.GL_TRIANGLES, 0, 3);
			if (prevCompare != GL11.GL_NONE) {
				GL11.glTexParameteri(GL11.GL_TEXTURE_2D, GL14.GL_TEXTURE_COMPARE_MODE, prevCompare);
			}
			return true;
		} finally {
			// restore
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
			if (blend) GL11.glEnable(GL11.GL_BLEND); else GL11.glDisable(GL11.GL_BLEND);
			if (cull) GL11.glEnable(GL11.GL_CULL_FACE); else GL11.glDisable(GL11.GL_CULL_FACE);
			GL11.glScissor(sc[0], sc[1], sc[2], sc[3]);
			if (scissor) GL11.glEnable(GL11.GL_SCISSOR_TEST); else GL11.glDisable(GL11.GL_SCISSOR_TEST);
			if (stencil) GL11.glEnable(GL11.GL_STENCIL_TEST); else GL11.glDisable(GL11.GL_STENCIL_TEST);
		}
	}
}
