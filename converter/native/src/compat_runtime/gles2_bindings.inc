// GLES 2.0 guest callouts. Included inside Forwarder::registerBindings so the
// callbacks share the same checked guest memory and EAGL state as GLES 1.
// Signatures and pointer ownership: https://www.khronos.org/files/opengles20-reference-card.pdf
// Explicit shader lengths: https://registry.khronos.org/OpenGL-Refpages/es2.0/glShaderSource.xml

// Simple object and program operations with one or two integer arguments.
for (const char *name : {"glCompileShader", "glDeleteShader", "glDeleteProgram", "glLinkProgram",
                         "glUseProgram", "glValidateProgram", "glEnableVertexAttribArray",
                         "glDisableVertexAttribArray", "glGenerateMipmap"}) {
    bind(std::string("_") + name, "gles2-forward-object", [&impl, name](Args &a) {
        if (std::strcmp(name, "glEnableVertexAttribArray") == 0 ||
            std::strcmp(name, "glDisableVertexAttribArray") == 0) {
            if (a.u32(0) >= 256) { impl.refuse(name, "attribute index is out of range"); return true; }
            impl.vertexAttribs[a.u32(0)].enabled = name[2] == 'E';
        }
        impl.call<void (*)(GLuint)>(name, a.u32(0));
        return true;
    });
}
for (const char *name : {"glAttachShader", "glDetachShader", "glBlendEquationSeparate"}) {
    bind(std::string("_") + name, "gles2-forward-pair", [&impl, name](Args &a) {
        impl.call<void (*)(GLuint, GLuint)>(name, a.u32(0), a.u32(1));
        return true;
    });
}
for (const char *name : {"glCreateShader", "glCreateProgram"}) {
    bind(std::string("_") + name, "gles2-forward-create", [&impl, name](Args &a) {
        if (std::strcmp(name, "glCreateProgram") == 0) {
            auto *fn = impl.symbol<GLuint (*)()>(name);
            if (!fn) { impl.refuse(name); a.registers.r[0] = 0; return true; }
            a.registers.r[0] = fn();
        } else {
            auto *fn = impl.symbol<GLuint (*)(GLenum)>(name);
            if (!fn) { impl.refuse(name); a.registers.r[0] = 0; return true; }
            a.registers.r[0] = fn(a.enumValue(0));
        }
        ++impl.forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
}

bind("_glShaderSource", "gles2-forward-shader-source", [&impl](Args &a) {
    const GLsizei count = a.i32(1);
    if (count < 0 || count > 4096 || (count != 0 && a.pointer(2) == 0)) {
        impl.refuse("glShaderSource", "invalid source count or pointer array");
        return true;
    }
    std::vector<std::string> source;
    std::vector<const char *> pointers;
    std::vector<GLint> lengths;
    source.reserve(static_cast<std::size_t>(count));
    lengths.reserve(static_cast<std::size_t>(count));
    std::size_t total = 0;
    constexpr std::size_t maxSourceBytes = 64u * 1024u * 1024u;
    for (GLsizei i = 0; i < count; ++i) {
        const auto pointerSlot = static_cast<std::uint64_t>(a.pointer(2)) + static_cast<std::uint64_t>(i) * 4u;
        const auto lengthSlot = static_cast<std::uint64_t>(a.pointer(3)) + static_cast<std::uint64_t>(i) * 4u;
        GuestAddress address = 0;
        GLint length = -1;
        if (pointerSlot > 0xffffffffULL ||
            !a.memory.read(static_cast<GuestAddress>(pointerSlot), &address, sizeof(address)) ||
            (a.pointer(3) != 0 && (lengthSlot > 0xffffffffULL ||
             !a.memory.read(static_cast<GuestAddress>(lengthSlot), &length, sizeof(length)))) || address == 0) {
            impl.refuse("glShaderSource", "unreadable source pointer or length");
            return true;
        }
        std::string chunk;
        if (length < 0) {
            if (!impl.guestString(a.memory, address, chunk, "glShaderSource")) return true;
        } else {
            if (static_cast<std::size_t>(length) > maxSourceBytes - total) {
                impl.refuse("glShaderSource", "shader source exceeds the safety limit");
                return true;
            }
            chunk.resize(static_cast<std::size_t>(length));
            if (length > 0 && !a.memory.read(address, chunk.data(), chunk.size())) {
                impl.refuse("glShaderSource", "shader source range is unreadable");
                return true;
            }
        }
        if (chunk.size() > maxSourceBytes - total) {
            impl.refuse("glShaderSource", "shader source exceeds the safety limit");
            return true;
        }
        total += chunk.size();
        lengths.push_back(static_cast<GLint>(chunk.size()));
        source.push_back(std::move(chunk));
    }
    pointers.reserve(source.size());
    for (const auto &chunk : source) pointers.push_back(chunk.data());
    impl.call<void (*)(GLuint, GLsizei, const char *const *, const GLint *)>(
        "glShaderSource", a.u32(0), count, pointers.empty() ? nullptr : pointers.data(),
        lengths.empty() ? nullptr : lengths.data());
    return true;
});

for (const char *name : {"glGetAttribLocation", "glGetUniformLocation"}) {
    bind(std::string("_") + name, "gles2-forward-location", [&impl, name](Args &a) {
        std::string text;
        if (!impl.guestString(a.memory, a.pointer(1), text, name)) {
            a.registers.r[0] = 0xffffffffu;
            return true;
        }
        auto *fn = impl.symbol<GLint (*)(GLuint, const char *)>(name);
        if (!fn) { impl.refuse(name); a.registers.r[0] = 0xffffffffu; return true; }
        a.registers.r[0] = static_cast<std::uint32_t>(fn(a.u32(0), text.c_str()));
        ++impl.forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
        return true;
    });
}
bind("_glBindAttribLocation", "gles2-forward-bind-attribute-location", [&impl](Args &a) {
    std::string text;
    if (impl.guestString(a.memory, a.pointer(2), text, "glBindAttribLocation"))
        impl.call<void (*)(GLuint, GLuint, const char *)>("glBindAttribLocation", a.u32(0), a.u32(1), text.c_str());
    return true;
});

for (const char *name : {"glGetShaderiv", "glGetProgramiv"}) {
    bind(std::string("_") + name, "gles2-forward-object-query", [&impl, name](Args &a) {
        if (!a.memory.contains(a.pointer(2), sizeof(GLint), MemoryPermission::Write)) {
            impl.refuse(name, "query output is not writable");
            return true;
        }
        GLint result = 0;
        // An invalid driver query leaves the caller's output untouched. Seed
        // the staging value from readable guest memory when available.
        (void)a.memory.read(a.pointer(2), &result, sizeof(result));
        if (impl.call<void (*)(GLuint, GLenum, GLint *)>(name, a.u32(0), a.enumValue(1), &result))
            (void)a.memory.write(a.pointer(2), &result, sizeof(result));
        return true;
    });
}
for (const char *name : {"glGetShaderInfoLog", "glGetProgramInfoLog", "glGetShaderSource"}) {
    bind(std::string("_") + name, "gles2-forward-text-query", [&impl, name](Args &a) {
        const GLsizei size = a.i32(1);
        constexpr GLsizei maxText = 16 * 1024 * 1024;
        if (size < 0 || size > maxText ||
            (size > 0 && !a.memory.contains(a.pointer(3), static_cast<std::size_t>(size), MemoryPermission::Write)) ||
            (a.pointer(2) != 0 && !a.memory.contains(a.pointer(2), sizeof(GLsizei), MemoryPermission::Write))) {
            impl.refuse(name, "invalid or unwritable text-query output");
            return true;
        }
        std::vector<char> text(static_cast<std::size_t>(size));
        GLsizei length = -1;
        // Capture the number of bytes actually written even if the guest did
        // not ask for it; bytes beyond the terminating NUL must remain intact.
        if (impl.call<void (*)(GLuint, GLsizei, GLsizei *, char *)>(
                name, a.u32(0), size, &length, text.empty() ? nullptr : text.data()) &&
            length >= 0 && (size == 0 || length < size)) {
            if (!text.empty()) (void)a.memory.write(a.pointer(3), text.data(), static_cast<std::size_t>(length) + 1u);
            if (a.pointer(2)) (void)a.memory.write(a.pointer(2), &length, sizeof(length));
        }
        return true;
    });
}

for (const char *name : {"glUniform1i", "glUniform2i", "glUniform3i", "glUniform4i"}) {
    const int n = name[9] - '0';
    bind(std::string("_") + name, "gles2-forward-uniform-int", [&impl, name, n](Args &a) {
        switch (n) {
        case 1: impl.call<void (*)(GLint, GLint)>(name, a.i32(0), a.i32(1)); break;
        case 2: impl.call<void (*)(GLint, GLint, GLint)>(name, a.i32(0), a.i32(1), a.i32(2)); break;
        case 3: impl.call<void (*)(GLint, GLint, GLint, GLint)>(name, a.i32(0), a.i32(1), a.i32(2), a.i32(3)); break;
        case 4: impl.call<void (*)(GLint, GLint, GLint, GLint, GLint)>(name, a.i32(0), a.i32(1), a.i32(2), a.i32(3), a.i32(4)); break;
        default: break;
        }
        return true;
    });
}
for (const char *name : {"glUniform1f", "glUniform2f", "glUniform3f", "glUniform4f",
                         "glVertexAttrib1f", "glVertexAttrib2f", "glVertexAttrib3f", "glVertexAttrib4f"}) {
    const int n = name[std::strncmp(name, "glUniform", 9) == 0 ? 9 : 14] - '0';
    bind(std::string("_") + name, "gles2-forward-uniform-float", [&impl, name, n](Args &a) {
        switch (n) {
        case 1: impl.call<void (*)(GLint, GLfloat)>(name, a.i32(0), a.f(1)); break;
        case 2: impl.call<void (*)(GLint, GLfloat, GLfloat)>(name, a.i32(0), a.f(1), a.f(2)); break;
        case 3: impl.call<void (*)(GLint, GLfloat, GLfloat, GLfloat)>(name, a.i32(0), a.f(1), a.f(2), a.f(3)); break;
        case 4: impl.call<void (*)(GLint, GLfloat, GLfloat, GLfloat, GLfloat)>(name, a.i32(0), a.f(1), a.f(2), a.f(3), a.f(4)); break;
        default: break;
        }
        return true;
    });
}

// Uniform vectors/matrices use a direct checked host view when aligned. An
// unaligned guest pointer is copied to aligned temporary storage before the
// host driver sees it. Zero counts do not dereference a guest pointer.
for (const char *name : {"glUniform1fv", "glUniform2fv", "glUniform3fv", "glUniform4fv",
                         "glUniform1iv", "glUniform2iv", "glUniform3iv", "glUniform4iv",
                         "glUniformMatrix2fv", "glUniformMatrix3fv", "glUniformMatrix4fv"}) {
    const bool matrix = std::strncmp(name, "glUniformMatrix", 15) == 0;
    const int width = name[matrix ? 15 : 9] - '0';
    const int components = matrix ? width * width : width;
    bind(std::string("_") + name, "gles2-forward-uniform-array", [&impl, name, matrix, components](Args &a) {
        const GLsizei count = a.i32(1);
        const auto bytes = count < 0 ? 0ULL : static_cast<std::uint64_t>(count) * components * 4u;
        if (count < 0 || bytes > 64u * 1024u * 1024u) {
            impl.refuse(name, "invalid uniform count");
            return true;
        }
        const auto address = a.pointer(matrix ? 3 : 2);
        const void *host = nullptr;
        std::vector<std::uint32_t> aligned;
        if (bytes != 0) {
            host = impl.hostAddress(a.memory, address, static_cast<std::size_t>(bytes), name);
            if (!host) return true;
            if (reinterpret_cast<std::uintptr_t>(host) % alignof(std::uint32_t) != 0) {
                aligned.resize(static_cast<std::size_t>(bytes / 4u));
                std::memcpy(aligned.data(), host, static_cast<std::size_t>(bytes));
                host = aligned.data();
            }
        }
        if (matrix) {
            impl.call<void (*)(GLint, GLsizei, unsigned char, const GLfloat *)>(
                name, a.i32(0), count, static_cast<unsigned char>(a.u32(2) != 0),
                static_cast<const GLfloat *>(host));
        } else if (name[10] == 'i') {
            impl.call<void (*)(GLint, GLsizei, const GLint *)>(name, a.i32(0), count,
                                                               static_cast<const GLint *>(host));
        } else {
            impl.call<void (*)(GLint, GLsizei, const GLfloat *)>(name, a.i32(0), count,
                                                                 static_cast<const GLfloat *>(host));
        }
        return true;
    });
}

bind("_glVertexAttribPointer", "gles2-forward-attribute-pointer", [&impl](Args &a) {
    const auto index = a.u32(0);
    if (index >= 256) { impl.refuse("glVertexAttribPointer", "attribute index is out of range"); return true; }
    auto &attribute = impl.vertexAttribs[index];
    attribute.size = a.i32(1);
    attribute.type = a.enumValue(2);
    attribute.normalized = static_cast<unsigned char>(a.u32(3) != 0);
    attribute.stride = a.i32(4);
    attribute.pointer = a.pointer(5);
    attribute.buffer = static_cast<GLuint>(impl.boundArrayBuffer);
    if (attribute.buffer != 0) {
        impl.call<void (*)(GLuint, GLint, GLenum, unsigned char, GLsizei, const void *)>(
            "glVertexAttribPointer", index, attribute.size, attribute.type, attribute.normalized,
            attribute.stride, reinterpret_cast<const void *>(static_cast<std::uintptr_t>(attribute.pointer)));
    }
    return true;
});

// GLES2 core names for the same framebuffer/renderbuffer operations that ES1
// exposes with the OES suffix. A drawable-backed color attachment maps to the
// EGL surface's default framebuffer; texture FBOs remain ordinary host FBOs.
for (const char *name : {"glGenFramebuffers", "glGenRenderbuffers"}) {
    bind(std::string("_") + name, "gles2-forward-gen-object", [&impl, name](Args &a) {
        impl.generateNames(a, name);
        return true;
    });
}
for (const char *name : {"glDeleteFramebuffers", "glDeleteRenderbuffers"}) {
    bind(std::string("_") + name, "gles2-forward-delete-object", [&impl, name](Args &a) {
        impl.deleteNames(a, name, [&impl, name](GLuint value) {
            if (std::strcmp(name, "glDeleteFramebuffers") == 0) impl.deletedFramebuffer(value);
            else impl.deletedRenderbuffer(value);
        });
        return true;
    });
}
bind("_glBindRenderbuffer", "gles2-forward-bind-renderbuffer", [&impl](Args &a) {
    if (impl.call<void (*)(GLenum, GLuint)>("glBindRenderbuffer", a.enumValue(0), a.u32(1)) &&
        a.enumValue(0) == GL_RENDERBUFFER_OES) impl.boundRenderbuffer = a.i32(1);
    return true;
});
bind("_glRenderbufferStorage", "gles2-forward-renderbuffer-storage", [&impl](Args &a) {
    if (a.enumValue(0) == GL_RENDERBUFFER_OES && impl.drawableRenderbuffers.count(impl.boundRenderbuffer)) {
        impl.refuse("glRenderbufferStorage", "replacing EAGL drawable storage is unsupported");
        return true;
    }
    impl.call<void (*)(GLenum, GLenum, GLsizei, GLsizei)>("glRenderbufferStorage",
        a.enumValue(0), a.enumValue(1), a.i32(2), a.i32(3));
    return true;
});
bind("_glGetRenderbufferParameteriv", "gles2-forward-renderbuffer-query", [&impl](Args &a) {
    impl.queryRenderbuffer(a, "glGetRenderbufferParameteriv");
    return true;
});
bind("_glBindFramebuffer", "gles2-forward-bind-framebuffer", [&impl](Args &a) {
    const auto target = a.enumValue(0); const auto name = a.i32(1);
    const auto hostName = target == GL_FRAMEBUFFER_OES && name != 0 && impl.drawableFramebuffers.count(name) ? 0u : a.u32(1);
    if (impl.call<void (*)(GLenum, GLuint)>("glBindFramebuffer", target, hostName) && target == GL_FRAMEBUFFER_OES)
        impl.boundFramebuffer = name;
    return true;
});
bind("_glFramebufferRenderbuffer", "gles2-forward-framebuffer-renderbuffer", [&impl](Args &a) {
    const auto target = a.enumValue(0), attachment = a.enumValue(1), rbTarget = a.enumValue(2);
    const auto renderbuffer = a.i32(3);
    const bool validTarget = target == GL_FRAMEBUFFER_OES && (renderbuffer == 0 || rbTarget == GL_RENDERBUFFER_OES) && impl.boundFramebuffer != 0;
    if (validTarget && attachment == GL_COLOR_ATTACHMENT0_OES && renderbuffer != 0 &&
        impl.drawableRenderbuffers.count(renderbuffer)) {
        impl.drawableFramebuffers[impl.boundFramebuffer] = impl.drawableRenderbuffers.at(renderbuffer);
        impl.call<void (*)(GLenum, GLuint)>("glBindFramebuffer", GL_FRAMEBUFFER_OES, 0);
        return true;
    }
    if (validTarget && attachment == GL_COLOR_ATTACHMENT0_OES) impl.detachCurrentDrawableFramebuffer();
    impl.call<void (*)(GLenum, GLenum, GLenum, GLuint)>("glFramebufferRenderbuffer", target, attachment, rbTarget, a.u32(3));
    return true;
});
bind("_glFramebufferTexture2D", "gles2-forward-framebuffer-texture", [&impl](Args &a) {
    const auto target = a.enumValue(0), attachment = a.enumValue(1), texTarget = a.enumValue(2);
    const auto texture = a.u32(3); const auto level = a.i32(4);
    if (target == GL_FRAMEBUFFER_OES && attachment == GL_COLOR_ATTACHMENT0_OES && impl.boundFramebuffer != 0 &&
        Impl::validFramebufferTexture(texTarget, texture, level)) impl.detachCurrentDrawableFramebuffer();
    impl.call<void (*)(GLenum, GLenum, GLenum, GLuint, GLint)>("glFramebufferTexture2D", target, attachment, texTarget, texture, level);
    return true;
});
bind("_glCheckFramebufferStatus", "gles2-forward-framebuffer-status", [&impl](Args &a) {
    if (a.enumValue(0) == GL_FRAMEBUFFER_OES && impl.boundFramebuffer != 0 && impl.drawableFramebuffers.count(impl.boundFramebuffer)) {
        a.registers.r[0] = GL_FRAMEBUFFER_COMPLETE_OES;
    } else if (auto *fn = impl.symbol<GLenum (*)(GLenum)>("glCheckFramebufferStatus")) {
        a.registers.r[0] = fn(a.enumValue(0));
        ++impl.forwarded;
        gProgress.forwardedCalls.fetch_add(1, std::memory_order_relaxed);
    } else {
        impl.refuse("glCheckFramebufferStatus");
        a.registers.r[0] = 0;
    }
    return true;
});

for (const char *name : {"glBlendEquation", "glClearStencil", "glStencilMask"}) {
    bind(std::string("_") + name, "gles2-forward-single-state", [&impl, name](Args &a) {
        impl.call<void (*)(GLuint)>(name, a.u32(0)); return true;
    });
}
for (const char *name : {"glClearDepthf", "glSampleCoverage"}) {
    bind(std::string("_") + name, "gles2-forward-float-state", [&impl, name](Args &a) {
        if (std::strcmp(name, "glClearDepthf") == 0) impl.call<void (*)(GLfloat)>(name, a.f(0));
        else impl.call<void (*)(GLfloat, unsigned char)>(name, a.f(0), static_cast<unsigned char>(a.u32(1) != 0));
        return true;
    });
}
bind("_glDepthRangef", "gles2-forward-depth-range", [&impl](Args &a) {
    impl.call<void (*)(GLfloat, GLfloat)>("glDepthRangef", a.f(0), a.f(1)); return true;
});
bind("_glBlendFuncSeparate", "gles2-forward-blend-separate", [&impl](Args &a) {
    impl.call<void (*)(GLenum, GLenum, GLenum, GLenum)>("glBlendFuncSeparate", a.enumValue(0), a.enumValue(1), a.enumValue(2), a.enumValue(3)); return true;
});
bind("_glStencilFunc", "gles2-forward-stencil-func", [&impl](Args &a) {
    impl.call<void (*)(GLenum, GLint, GLuint)>("glStencilFunc", a.enumValue(0), a.i32(1), a.u32(2)); return true;
});
bind("_glStencilOp", "gles2-forward-stencil-op", [&impl](Args &a) {
    impl.call<void (*)(GLenum, GLenum, GLenum)>("glStencilOp", a.enumValue(0), a.enumValue(1), a.enumValue(2)); return true;
});
bind("_glTexParameterf", "gles2-forward-texture-float", [&impl](Args &a) {
    impl.call<void (*)(GLenum, GLenum, GLfloat)>("glTexParameterf", a.enumValue(0), a.enumValue(1), a.f(2)); return true;
});
