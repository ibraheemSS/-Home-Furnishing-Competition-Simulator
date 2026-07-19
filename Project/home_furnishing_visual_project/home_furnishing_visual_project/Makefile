CC := gcc
CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O2 -g -Iinclude
LDFLAGS :=
BIN_DIR := bin
SRC_DIR := src
OBJ_DIR := build

SIM_SRCS := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/util.c \
	$(SRC_DIR)/ipc.c \
	$(SRC_DIR)/signals.c \
	$(SRC_DIR)/furniture.c \
	$(SRC_DIR)/team_member.c \
	$(SRC_DIR)/master.c

SIM_OBJS := $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SIM_SRCS))

.PHONY: all clean run fast openmp debug visualizer graphics dirs

all: dirs $(BIN_DIR)/furnish

dirs:
	@mkdir -p $(BIN_DIR) $(OBJ_DIR) logs

$(BIN_DIR)/furnish: $(SIM_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c include/*.h | dirs
	$(CC) $(CFLAGS) -c $< -o $@

openmp: CFLAGS += -DUSE_OPENMP -fopenmp
openmp: LDFLAGS += -fopenmp
openmp: clean dirs $(BIN_DIR)/furnish

# OpenGL is optional. Requires freeglut/OpenGL development packages.
visualizer: dirs
	$(CC) $(CFLAGS) -o $(BIN_DIR)/visualizer $(SRC_DIR)/visualizer.c -Iinclude -lglut -lGLU -lGL -lm

graphics: all visualizer

debug: CFLAGS := -std=c11 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Wall -Wextra -Wpedantic -O0 -g3 -Iinclude

debug: clean dirs $(BIN_DIR)/furnish

run: all
	./$(BIN_DIR)/furnish config/sample.conf

fast: all
	./$(BIN_DIR)/furnish config/fast.conf

clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)
