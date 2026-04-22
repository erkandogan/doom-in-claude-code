# doom-in-claude — doomgeneric backend build.
#
# Fetches the upstream doomgeneric source (if missing), compiles the Doom
# engine objects, links them with our wrapper.c into ./doomd.
#
# Targets:
#   all (default)   build ./doomd
#   fetch           clone doomgeneric
#   clean           remove intermediate artifacts
#   distclean       also remove doomgeneric/ and the binary

DOOMGEN_REPO := https://github.com/ozkl/doomgeneric.git
DOOMGEN_DIR  := doomgeneric
DOOMGEN_SRC  := $(DOOMGEN_DIR)/doomgeneric

CC       ?= cc
CFLAGS   += -O2 -Wall -Wno-pointer-sign -Wno-unused-variable -Wno-unused-function \
            -Wno-parentheses -DSNDSERV -DNORMALUNIX -I$(DOOMGEN_DIR)
LDFLAGS  += -lm -lpthread

# Explicit source list lifted from upstream's Makefile (see
# doomgeneric/doomgeneric/Makefile). This is safer than a wildcard because
# the repo contains optional music / platform backends (Allegro, SDL,
# emscripten, win32) whose sources won't compile without their SDKs. We
# pull in only the portable Doom engine + doomgeneric.c itself — our
# wrapper.c replaces every doomgeneric_<backend>.c.
DOOM_SRCS := dummy am_map doomdef doomstat dstrings d_event d_items d_iwad \
             d_loop d_main d_mode d_net f_finale f_wipe g_game hu_lib hu_stuff \
             info i_cdmus i_endoom i_joystick i_scale i_sound i_system i_timer \
             memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu \
             m_misc m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights \
             p_map p_maputl p_mobj p_plats p_pspr p_saveg p_setup p_sight \
             p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw r_main \
             r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff \
             s_sound tables v_video wi_stuff w_checksum w_file w_main w_wad \
             z_zone w_file_stdc i_input i_video doomgeneric
SRCS := $(addprefix $(DOOMGEN_SRC)/,$(addsuffix .c,$(DOOM_SRCS)))

OBJS := $(SRCS:.c=.o) wrapper.o

all: doomd

fetch: $(DOOMGEN_DIR)

$(DOOMGEN_DIR):
	@echo "Cloning doomgeneric..."
	git clone --depth 1 $(DOOMGEN_REPO) $(DOOMGEN_DIR)

wrapper.o: wrapper.c | $(DOOMGEN_DIR)
	$(CC) $(CFLAGS) -c wrapper.c -o wrapper.o

$(DOOMGEN_SRC)/%.o: $(DOOMGEN_SRC)/%.c | $(DOOMGEN_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

doomd: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o doomd
	@echo "Built ./doomd"

clean:
	rm -f wrapper.o doomd
	@if [ -d $(DOOMGEN_SRC) ]; then rm -f $(DOOMGEN_SRC)/*.o; fi

distclean: clean
	rm -rf $(DOOMGEN_DIR)

.PHONY: all fetch clean distclean
