package io.github.primerecomp;

import org.libsdl.app.SDLActivity;

// SDL's activity, with the libraries the runtime (libmain.so, which holds SDL_main) needs loaded first
public class PrimeActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"c++_shared", "SDL3", "avutil", "swscale", "avcodec", "avformat", "main"};
    }
}
