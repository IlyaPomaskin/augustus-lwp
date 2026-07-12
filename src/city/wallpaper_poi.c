#include "city/wallpaper_poi.h"

#include "building/building.h"
#include "building/type.h"
#include "city/view.h"
#include "core/log.h"
#include "core/random.h"
#include "map/grid.h"

#include <stdlib.h>
#include <string.h>

#define POI_SAMPLE_COUNT 20
#define POI_POOL_MAX 50
#define POI_MAX_CANDIDATES 1024
#define POI_EDGE_MARGIN 20
#define POI_MIN_SPACING 10
#define POI_CELL_TILES 15
#define POI_POP_SCORE_CAP 5
#define POI_POP_PER_POINT 500
#define POI_INDUSTRY_MIN 4
#define POI_INDUSTRY_DENSE 8
#define POI_INDUSTRY_SCORE 3
#define POI_INDUSTRY_SCORE_DENSE 5

#define POI_GRID_MAX ((GRID_SIZE + POI_CELL_TILES - 1) / POI_CELL_TILES)

typedef struct {
    int grid_offset;
    int score;
} wallpaper_poi;

typedef struct {
    int population;
    int best_pop;
    int best_pop_offset;
    int industry_count;
    int industry_offset;
} poi_cell;

static wallpaper_poi poi_list[POI_SAMPLE_COUNT];
static int poi_count;
static int poi_index;

static wallpaper_poi candidates[POI_MAX_CANDIDATES];
static int candidate_count;
static poi_cell cells[POI_GRID_MAX][POI_GRID_MAX];

static int landmark_score(building_type type)
{
    switch (type) {
        case BUILDING_COLOSSEUM:
        case BUILDING_HIPPODROME:
        case BUILDING_ARENA:
            return 8;
        case BUILDING_GRAND_TEMPLE_CERES:
        case BUILDING_GRAND_TEMPLE_NEPTUNE:
        case BUILDING_GRAND_TEMPLE_MERCURY:
        case BUILDING_GRAND_TEMPLE_MARS:
        case BUILDING_GRAND_TEMPLE_VENUS:
        case BUILDING_PANTHEON:
        case BUILDING_ORACLE:
            return 7;
        case BUILDING_SENATE:
        case BUILDING_GOVERNORS_PALACE:
            return 6;
        case BUILDING_GOVERNORS_VILLA:
        case BUILDING_LARGE_MAUSOLEUM:
        case BUILDING_CARAVANSERAI:
        case BUILDING_LIGHTHOUSE:
        case BUILDING_CITY_MINT:
        case BUILDING_MILITARY_ACADEMY:
            return 5;
        case BUILDING_THEATER:
        case BUILDING_AMPHITHEATER:
        case BUILDING_GLADIATOR_SCHOOL:
        case BUILDING_LARGE_TEMPLE_CERES:
        case BUILDING_LARGE_TEMPLE_NEPTUNE:
        case BUILDING_LARGE_TEMPLE_MERCURY:
        case BUILDING_LARGE_TEMPLE_MARS:
        case BUILDING_LARGE_TEMPLE_VENUS:
        case BUILDING_GOVERNORS_HOUSE:
        case BUILDING_TRIUMPHAL_ARCH:
        case BUILDING_OBELISK:
            return 4;
        case BUILDING_FORUM:
        case BUILDING_FORT_LEGIONARIES:
        case BUILDING_FORT_JAVELIN:
        case BUILDING_FORT_MOUNTED:
        case BUILDING_FORT_AUXILIA_INFANTRY:
        case BUILDING_FORT_ARCHERS:
            return 3;
        default:
            return 0;
    }
}

static int is_industry(building_type type)
{
    // Contiguous enum range: farms (100-105), raw materials (106-109),
    // basic workshops (110-114).
    if (type >= BUILDING_WHEAT_FARM && type <= BUILDING_POTTERY_WORKSHOP) {
        return 1;
    }
    switch (type) {
        case BUILDING_WAREHOUSE:
        case BUILDING_GRANARY:
        case BUILDING_DOCK:
        case BUILDING_WHARF:
        case BUILDING_GOLD_MINE:
        case BUILDING_SAND_PIT:
        case BUILDING_STONE_QUARRY:
        case BUILDING_CONCRETE_MAKER:
        case BUILDING_BRICKWORKS:
            return 1;
        default:
            return 0;
    }
}

static int footprint_center(const building *b)
{
    int centered = map_grid_add_delta(b->grid_offset, b->size / 2, b->size / 2);
    if (map_grid_is_valid_offset(centered)) {
        return centered;
    }
    return b->grid_offset;
}

static void add_candidate(int grid_offset, int score)
{
    if (candidate_count >= POI_MAX_CANDIDATES || score <= 0) {
        return;
    }
    if (!map_grid_is_valid_offset(grid_offset)) {
        return;
    }
    candidates[candidate_count].grid_offset = grid_offset;
    candidates[candidate_count].score = score;
    candidate_count++;
}

static int chebyshev(int a_offset, int b_offset)
{
    int ax = map_grid_offset_to_x(a_offset);
    int ay = map_grid_offset_to_y(a_offset);
    int bx = map_grid_offset_to_x(b_offset);
    int by = map_grid_offset_to_y(b_offset);
    int dx = ax > bx ? ax - bx : bx - ax;
    int dy = ay > by ? ay - by : by - ay;
    return dx > dy ? dx : dy;
}

static int compare_score_desc(const void *a, const void *b)
{
    const wallpaper_poi *pa = a;
    const wallpaper_poi *pb = b;
    return pb->score - pa->score;
}

static void collect_candidates(void)
{
    candidate_count = 0;
    memset(cells, 0, sizeof(cells));

    int total = building_count();
    for (int id = 1; id < total; id++) {
        building *b = building_get(id);
        if (b->state != BUILDING_STATE_IN_USE) {
            continue;
        }
        add_candidate(footprint_center(b), landmark_score(b->type));

        int cx = map_grid_offset_to_x(b->grid_offset) / POI_CELL_TILES;
        int cy = map_grid_offset_to_y(b->grid_offset) / POI_CELL_TILES;
        if (cx < 0 || cx >= POI_GRID_MAX || cy < 0 || cy >= POI_GRID_MAX) {
            continue;
        }
        poi_cell *cell = &cells[cx][cy];
        if (b->house_population > 0) {
            cell->population += b->house_population;
            if (b->house_population > cell->best_pop) {
                cell->best_pop = b->house_population;
                cell->best_pop_offset = b->grid_offset;
            }
        } else if (is_industry(b->type)) {
            if (cell->industry_count == 0) {
                cell->industry_offset = b->grid_offset;
            }
            cell->industry_count++;
        }
    }

    for (int cx = 0; cx < POI_GRID_MAX; cx++) {
        for (int cy = 0; cy < POI_GRID_MAX; cy++) {
            poi_cell *cell = &cells[cx][cy];
            if (cell->best_pop_offset) {
                int score = cell->population / POI_POP_PER_POINT;
                if (score > POI_POP_SCORE_CAP) {
                    score = POI_POP_SCORE_CAP;
                }
                add_candidate(cell->best_pop_offset, score);
            }
            if (cell->industry_count >= POI_INDUSTRY_MIN) {
                int dense = cell->industry_count >= POI_INDUSTRY_DENSE;
                add_candidate(cell->industry_offset,
                    dense ? POI_INDUSTRY_SCORE_DENSE : POI_INDUSTRY_SCORE);
            }
        }
    }
}

static int drop_edge_candidates(void)
{
    int width = map_grid_width();
    int height = map_grid_height();
    int kept = 0;
    for (int i = 0; i < candidate_count; i++) {
        int x = map_grid_offset_to_x(candidates[i].grid_offset);
        int y = map_grid_offset_to_y(candidates[i].grid_offset);
        int near_edge = x < POI_EDGE_MARGIN || y < POI_EDGE_MARGIN
            || x >= width - POI_EDGE_MARGIN || y >= height - POI_EDGE_MARGIN;
        if (!near_edge) {
            candidates[kept++] = candidates[i];
        }
    }
    candidate_count = kept;
    return kept;
}

static int build_pool(wallpaper_poi *pool)
{
    int pool_count = 0;
    for (int i = 0; i < candidate_count && pool_count < POI_POOL_MAX; i++) {
        int too_close = 0;
        for (int j = 0; j < pool_count; j++) {
            if (chebyshev(candidates[i].grid_offset, pool[j].grid_offset) < POI_MIN_SPACING) {
                too_close = 1;
                break;
            }
        }
        if (!too_close) {
            pool[pool_count++] = candidates[i];
        }
    }
    return pool_count;
}

static void sample_pool(wallpaper_poi *pool, int pool_count)
{
    if (pool_count <= POI_SAMPLE_COUNT) {
        for (int i = 0; i < pool_count; i++) {
            poi_list[i] = pool[i];
        }
        poi_count = pool_count;
    } else {
        for (int i = 0; i < POI_SAMPLE_COUNT; i++) {
            int r = random_between_from_stdlib(i, pool_count);
            wallpaper_poi tmp = pool[i];
            pool[i] = pool[r];
            pool[r] = tmp;
            poi_list[i] = pool[i];
        }
        poi_count = POI_SAMPLE_COUNT;
    }
    poi_index = 0;
}

static void scan(void)
{
    collect_candidates();
    drop_edge_candidates();
    qsort(candidates, candidate_count, sizeof(wallpaper_poi), compare_score_desc);
    wallpaper_poi pool[POI_POOL_MAX];
    int pool_count = build_pool(pool);
    sample_pool(pool, pool_count);
}

void wallpaper_poi_invalidate(void)
{
    poi_count = 0;
}

void wallpaper_poi_next(void)
{
    int fully_cycled = poi_index + 1 >= poi_count;
    if (poi_count == 0 || fully_cycled) {
        scan();
    } else {
        poi_index++;
    }
    if (poi_count == 0) {
        city_view_go_to_random_tile();
        return;
    }
    city_view_go_to_grid_offset(poi_list[poi_index].grid_offset);
    log_info("Wallpaper POI grid offset", 0, poi_list[poi_index].grid_offset);
}
