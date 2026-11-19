#pragma once
#include <stdint.h>

#define MAZE_MAX_CELLS 209

typedef struct {
    uint8_t walls[MAZE_MAX_CELLS]; // 上右下左四个墙位
    uint16_t path[MAZE_MAX_CELLS]; // 起点至终点的唯一路径
    uint16_t stars[5]; // 浅支路或主路上的星星
    uint16_t path_count;
    uint16_t position;
    uint8_t columns, rows;
    uint8_t collected; // 五颗星星的收集位图
} Maze;

static uint32_t MazeRandom(uint32_t* seed, uint32_t maximum) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed % maximum;
}

static int MazeNeighbor(const Maze* maze, int cell, int direction) {
    if (direction == 0) return cell - maze->columns;
    if (direction == 1) return cell + 1;
    if (direction == 2) return cell + maze->columns;
    return cell - 1;
}

static int MazeInside(const Maze* maze, int cell, int direction) {
    if (direction == 0) return cell >= maze->columns;
    if (direction == 1) return cell % maze->columns + 1 < maze->columns;
    if (direction == 2) return cell + maze->columns < maze->columns * maze->rows;
    return cell % maze->columns != 0;
}

static void MazeGenerate(Maze* maze, int columns, int rows, uint32_t seed) {
    uint16_t work[MAZE_MAX_CELLS];
    int16_t previous[MAZE_MAX_CELLS];
    uint8_t seen[MAZE_MAX_CELLS];
    const int count = columns * rows;
    const int goal = count - 1;
    maze->columns = (uint8_t)columns;
    maze->rows = (uint8_t)rows;
    maze->position = 0;
    maze->collected = 0;
    for (int i = 0; i < count; ++i) {
        maze->walls[i] = 15;
        seen[i] = 0;
        previous[i] = -1;
    }
    seen[0] = seen[goal] = 1;
    int length = 1;
    work[0] = 0;
    while (length) {
        const int cell = work[length - 1];
        int choices[4], options = 0;
        for (int d = 0; d < 4; ++d) {
            if (MazeInside(maze, cell, d) && !seen[MazeNeighbor(maze, cell, d)]) choices[options++] = d;
        }
        if (!options) {
            --length;
            continue;
        }
        const int direction = choices[MazeRandom(&seed, (uint32_t)options)];
        const int next = MazeNeighbor(maze, cell, direction);
        maze->walls[cell] &= (uint8_t)~(1 << direction);
        maze->walls[next] &= (uint8_t)~(1 << ((direction + 2) % 4));
        seen[next] = 1;
        work[length++] = (uint16_t)next;
    }
    // 终点最后以叶节点接入，避免出现必须经过终点的支路。
    const int direction = MazeRandom(&seed, 2) == 0 ? 0 : 3;
    const int entry = MazeNeighbor(maze, goal, direction);
    maze->walls[goal] &= (uint8_t)~(1 << direction);
    maze->walls[entry] &= (uint8_t)~(1 << ((direction + 2) % 4));
    int head = 0, tail = 1;
    work[0] = 0;
    previous[0] = 0;
    while (head < tail) {
        const int cell = work[head++];
        for (int d = 0; d < 4; ++d) {
            if (maze->walls[cell] & (1 << d)) continue;
            const int next = MazeNeighbor(maze, cell, d);
            if (previous[next] >= 0) continue;
            previous[next] = (int16_t)cell;
            work[tail++] = (uint16_t)next;
        }
    }
    maze->path_count = 0;
    for (int cell = goal;; cell = previous[cell]) {
        maze->path[maze->path_count++] = (uint16_t)cell;
        if (!cell) break;
    }
    for (int i = 0; i < maze->path_count / 2; ++i) {
        const uint16_t cell = maze->path[i];
        maze->path[i] = maze->path[maze->path_count - i - 1];
        maze->path[maze->path_count - i - 1] = cell;
    }
    for (int i = 0; i < count; ++i) seen[i] = 0;
    for (int i = 0; i < maze->path_count; ++i) seen[maze->path[i]] = 1;
    for (int star = 0; star < 5; ++star) {
        int best = -1, best_distance = 0x7fffffff;
        for (int i = 1; i + 1 < maze->path_count; ++i) {
            const int cell = maze->path[i];
            int distance = 6 * i - (star + 1) * (maze->path_count - 1);
            if (distance < 0) distance = -distance;
            for (int d = 0; d < 4; ++d) {
                if (maze->walls[cell] & (1 << d)) continue;
                const int next = MazeNeighbor(maze, cell, d);
                if (seen[next]) continue;
                if (distance < best_distance || (distance == best_distance && next < best)) {
                    best = next;
                    best_distance = distance;
                }
            }
        }
        if (best < 0) {
            // 浅支路不足时仅从主路补齐，不向深支路延伸。
            for (int i = 1; best < 0 && i + 1 < maze->path_count; ++i) {
                const int index = i * (maze->path_count - 1) / 6;
                if (index < 1 || index + 1 >= maze->path_count) continue;
                best = maze->path[index];
                for (int j = 0; j < star; ++j) if (maze->stars[j] == best) best = -1;
            }
        }
        maze->stars[star] = (uint16_t)best;
        seen[best] = 1;
    }
}

static int MazeMove(Maze* maze, int direction) {
    if (direction < 0 || direction > 3 || (maze->walls[maze->position] & (1 << direction))) return 0;
    maze->position = (uint16_t)MazeNeighbor(maze, maze->position, direction);
    for (int i = 0; i < 5; ++i) if (maze->stars[i] == maze->position) maze->collected |= (uint8_t)(1 << i);
    return 1;
}

static int MazeScore(const Maze* maze) {
    int score = 0;
    for (int i = 0; i < 5; ++i) if (maze->collected & (1 << i)) score += 20;
    return score;
}
