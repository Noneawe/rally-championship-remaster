package org.rally.championship;

import org.libsdl.app.SDLActivity;

/** SDL2 hosts the game: libSDL2.so and libmain.so (the port's rally). */
public class RallyActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }
}
