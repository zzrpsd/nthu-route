#include "Construct_2d_tree.h"

#include <boost/range/iterator_range_core.hpp>
#include <ext/type_traits.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <fstream>

#include "../flute/flute-function.h"
#include "../flute/flute4nthuroute.h"
#include "../grdb/RoutingComponent.h"
#include "../grdb/RoutingRegion.h"
#include "../misc/geometry.h"
#include "Range_router.h"
#include "Route_2pinnets.h"

using namespace std;

void Construct_2d_tree::init_2pin_list() {
    int i, netnum;

    netnum = rr_map.get_netNumber();
    for (i = 0; i < netnum; ++i) {
//Two pin nets group by net id. So for fetching the 2nd net's 2-pin net,
//you can fetch by net_2pin_list[2][i], where i is the id of 2-pin net.
        net_2pin_list.push_back(std::vector<Two_pin_element_2d>());
        bbox_2pin_list.push_back(std::vector<Two_pin_element_2d>());
    }
}

void Construct_2d_tree::init_flute() {
    net_flutetree.resize(rr_map.get_netNumber());
}

void Construct_2d_tree::bbox_route(Two_pin_list_2d& list, const double value) {

    double u_value = -1;
    if (value > 0) {
        u_value = 1;
    }
    for (Two_pin_element_2d& it : list) {
        Rectangle rect { it.pin1.x, it.pin2 };

        rect.frame([&](const Coordinate_2d& c1,const Coordinate_2d& c2) {
            bboxRouteStateMap.edge(c1, c2) = 1;
        });
    }

//check the flag of edges, if it is set to 1, then add demand on it.
    for (Two_pin_element_2d& it : list) {

        Rectangle rect { it.pin1.x, it.pin2 };

        rect.frame([&](const Coordinate_2d& c1,const Coordinate_2d& c2) {
            int& color=bboxRouteStateMap.edge(c1, c2);
            if (color==1) {
                congestion.congestionMap2d.edge(c1, c2).cur_cap += u_value;
                color=0;
            }
        });
    }

#ifdef DEBUG_BBOX
    print_cap("cur");
#endif
}

void Construct_2d_tree::walkL(const Coordinate_2d& a, const Coordinate_2d& b, std::function<void(const Coordinate_2d& e1, const Coordinate_2d& e2)> f) {
    {
        int ax = a.x;
        int bx = b.x;
        if (ax > bx) {
            std::swap(ax, bx);
        }
        for (int x = ax; x < bx; ++x) {
            f(Coordinate_2d { x, a.y }, Coordinate_2d { x + 1, a.y });
        }
    }
    int ay = a.y;
    int by = b.y;
    if (ay > by) {
        std::swap(ay, by);
    }
    for (int y = ay; y < by; ++y) {
        f(Coordinate_2d { b.x, y }, Coordinate_2d { b.x, y + 1 });
    }
}

/*
 input: start coordinate and end coordinate, and directions of L
 output: record the best L pattern into two_pin_L_path_global, and return the min max congestion value
 */
Monotonic_element Construct_2d_tree::L_pattern_max_cong(const Coordinate_2d& c1, const Coordinate_2d& c2, Two_pin_element_2d& two_pin_L_path, int net_id) {

    int distance;

    Monotonic_element max_path;
    max_path.max_cost = -1000000;
    max_path.total_cost = 0;
    max_path.net_cost = 0;
    max_path.distance = 0;
    max_path.via_num = 1;

    walkL(c1, c2, [&](const Coordinate_2d& a,const Coordinate_2d& b) {
        double temp = congestion.get_cost_2d(a,b, net_id, distance);
        max_path.total_cost += max(static_cast<double>(0), temp);
        max_path.distance += distance;
        two_pin_L_path.path.push_back(a);
        if (temp > max_path.max_cost)
        max_path.max_cost = temp;
    });

    two_pin_L_path.path.push_back(c2);
    return max_path;
}

/*
 input: two coordinates
 output: record the L_pattern in the path, and the path is min max congestioned
 */

void Construct_2d_tree::L_pattern_route(const Coordinate_2d& c1, const Coordinate_2d& c2, Two_pin_element_2d& two_pin_L_path, int net_id) {
    Two_pin_element_2d path1;
    Monotonic_element max_cong_path1;

    max_cong_path1 = L_pattern_max_cong(c1, c2, path1, net_id);

    if (!c1.isAligned(c2)) {
        Two_pin_element_2d path2;
        Monotonic_element max_cong_path2;
        max_cong_path2 = L_pattern_max_cong(c2, c1, path2, net_id);
        if (max_cong_path1 < max_cong_path2) {
            two_pin_L_path = path1;
        } else {
            two_pin_L_path = path2;
        }
    } else {
        two_pin_L_path = path1;
    }

    two_pin_L_path.pin1 = two_pin_L_path.path[0];
    two_pin_L_path.pin2 = two_pin_L_path.path.back();
    two_pin_L_path.net_id = net_id;
}

//generate the congestion map by Flute with wirelength driven mode
void Construct_2d_tree::gen_FR_congestion_map() {
    //a struct, defined by Flute library

    for (int& i : bboxRouteStateMap.all()) {
        i = -1;
    }

    congestion.init_2d_map(rr_map);          //initial congestion map: calculating every edge's capacity
    init_2pin_list();       //initial 2-pin net container
    init_flute();           //initial the information of pin's coordinate and group by net for flute

    /*assign 0.5 demand to each net*/
#ifdef MESSAGE
    printf("bbox routing start...\n");
#endif	

//for storing the RSMT which returned by flute
    Flute netRoutingTreeRouter;
    std::vector<Tree> flutetree(rr_map.get_netNumber());

//Get every net's possible RSMT by flute, then use it to calculate the possible congestion
//In this section, we won't get a real routing result, but a possible congestion information.
    for (int i = 0; i < rr_map.get_netNumber(); ++i) {	//i:net id

#ifdef DEBUG_BBOX
        printf("bbox route net %d start...pin_num=%d\n",i,rr_map.get_netPinNumber(i));
#endif

        Tree& tree = flutetree[i];
//call flute to gen steiner tree and put the result in flutetree[]
        netRoutingTreeRouter.routeNet(rr_map.get_nPin(i), tree);

//The total node # in a tree, those nodes include pin and steiner point
//And it is defined as ((2 * degree of a tree) - 2) by the authors of flute
        tree.number = 2 * tree.deg - 2;	//add 0403

        /*2-pin bounding box assign demand 0.5, remember not to repeat the same net*/
        for (int j = 0; j < tree.number; ++j) {
            Branch& branch = tree.branch[j];
            //for all pins and steiner points

            Two_pin_element_2d two_pin;
            two_pin.pin1.x = (int) branch.x;
            two_pin.pin1.y = (int) branch.y;
            two_pin.pin2.x = (int) tree.branch[branch.n].x;
            two_pin.pin2.y = (int) tree.branch[branch.n].y;
            two_pin.net_id = i;
            if (two_pin.pin1 != two_pin.pin2) {
                bbox_2pin_list[i].push_back(std::move(two_pin));
            }

        }

        bbox_route(bbox_2pin_list[i], 0.5);
    }
#ifdef DEBUG1
    printf("bbox routing complete\n");
    print_cap("max");
    print_cap("cur");
#endif	
#ifdef MESSAGE
    printf("L-shaped pattern routing start...\n");
#endif		

//sort net by their bounding box size, then by their pin number
    vector<const Net*> sort_net;
    for (int i = 0; i < rr_map.get_netNumber(); ++i) {
        sort_net.push_back(&rr_map.get_netList()[i]);
    }
    sort(sort_net.begin(), sort_net.end(), [&]( const Net* a, const Net* b ) {return Net::comp_net(*a,*b);});

//Now begins the initial routing by pattern routing
//Edge shifting will also be applied to the routing.
    for (const Net* it : sort_net) {
        int netId = it->id;
        Tree& ftreeId = flutetree[netId];
        edge_shifting(ftreeId);
        global_flutetree = ftreeId;

        std::vector<int> flute_order(2 * ftreeId.deg - 2);
        for (int i = global_flutetree.number - 1; i >= 0; --i) {
            flute_order[i] = i;
        }

        net_flutetree[netId] = ftreeId;

        /*remove demand*/
        bbox_route(bbox_2pin_list[netId], -0.5);

        for (int k = 0; k < ftreeId.number; ++k) {

            Branch& branch = ftreeId.branch[flute_order[k]];
            int x1 = (int) branch.x;
            int y1 = (int) branch.y;
            int x2 = (int) ftreeId.branch[branch.n].x;
            int y2 = (int) ftreeId.branch[branch.n].y;
            if (!(x1 == x2 && y1 == y2)) {
                /*choose the L-shape with lower congestion to assign new demand 1*/
                Two_pin_element_2d L_path;
                L_pattern_route(Coordinate_2d { x1, y1 }, Coordinate_2d { x2, y2 }, L_path, netId);

                /*insert 2pin_path into this net*/
                net_2pin_list[netId].push_back(L_path);
                NetDirtyBit[L_path.net_id] = true;
                congestion.update_congestion_map_insert_two_pin_net(L_path);
#ifdef DEBUG_LROUTE
                print_path(*L_path);
#endif
            }
        }

    }

#ifdef MESSAGE
    printf("generate L-shape congestion map in stage1 successfully\n");
#endif	

#ifdef DEBUG1
    print_cap("cur");
#endif
    congestion.cal_max_overflow();

}

//=====================edge shifting=============================
//Return the smaller cost of L-shape path or the only one cost of flap path
double Construct_2d_tree::compute_L_pattern_cost(const Coordinate_2d& c1, const Coordinate_2d& c2, int net_id) {
    Two_pin_element_2d path1;
    Two_pin_element_2d path2;

    Monotonic_element max_cong_path1 = L_pattern_max_cong(c1, c2, path1, net_id);
    if (c1 == c2) {
        return max_cong_path1.total_cost;
    }
    Monotonic_element max_cong_path2 = L_pattern_max_cong(c2, c1, path2, net_id);

    if (max_cong_path1 < max_cong_path2) {
        return max_cong_path1.total_cost;
    } else {
        return max_cong_path2.total_cost;
    }

}

Vertex_flute_ptr Construct_2d_tree::findY(Vertex_flute& a, std::function<bool(const int& i, const int& j)> test) {
    Vertex_flute_ptr cur = &a;
    while (cur->type != PIN) {
        Vertex_flute_ptr find = *(cur->neighbor.begin());
        for (vector<Vertex_flute_ptr>::iterator nei = cur->neighbor.begin() + 1; nei != cur->neighbor.end(); ++nei) {
            if (test((*nei)->c.y, find->c.y)) {
                find = *nei;
            }
        }
        cur = find;
        if ((cur->c.y == a.c.y) || (cur->c.x != a.c.x)) {	//no neighboring vertex next to  a
            break;
        }
    }
    return cur;
}

Vertex_flute_ptr Construct_2d_tree::findX(Vertex_flute& a, std::function<bool(const int& i, const int& j)> test) {
    Vertex_flute_ptr cur = &a;
    while (cur->type != PIN) {
        Vertex_flute_ptr find = *(cur->neighbor.begin());
        for (vector<Vertex_flute_ptr>::iterator nei = cur->neighbor.begin() + 1; nei != cur->neighbor.end(); ++nei) {
            if (test((*nei)->c.x, find->c.x)) {
                find = *nei;
            }
        }
        cur = find;
        if (cur->c.x == a.c.x || cur->c.y != a.c.y) {	//no neighboring vertex in the right of a
            break;
        }
    }
    return cur;
}

void Construct_2d_tree::find_saferange(Vertex_flute& a, Vertex_flute& b, int *low, int *high, int dir) {
    auto greater = [&](const int& i,const int& j) {
        return i > j;
    };
    auto less = [&](const int& i,const int& j) {
        return i < j;
    };
//Horizontal edge doing vertical shifting
    if (dir == HOR) {
        *high = std::min(*high, findY(a, greater)->c.y);
        *high = std::min(*high, findY(b, greater)->c.y);
        *low = std::max(*low, findY(a, less)->c.y);
        *low = std::max(*low, findY(b, less)->c.y);
    } else {
        *high = min(*high, findX(a, greater)->c.x);
        *high = min(*high, findX(b, greater)->c.x);
        *low = max(*low, findX(a, less)->c.x);
        *low = max(*low, findX(b, less)->c.x);
    }
}

void Construct_2d_tree::merge_vertex(Vertex_flute& keep, Vertex_flute& deleted) {

    for (Vertex_flute_ptr nei : deleted.neighbor) {
        if (nei != &keep) {	//nei is not keep itself
            keep.neighbor.push_back(nei);	//add deleted's neighbor to keep
            for (Vertex_flute_ptr& find : nei->neighbor) {
                if (find->c == deleted.c) {	//neighbor[find] equals to deleted
                    find = &keep;	//replace its neighbor as keep
                    break;
                }
            }
        }
    }
    deleted.type = DELETED;
}

void Construct_2d_tree::move_edge_hor(Vertex_flute& a, int best_pos, Vertex_flute& b, Vertex_flute_ptr& overlap_a, std::function<bool(const int& i, const int& j)> test) {

    vector<Vertex_flute_ptr> st_pt;
    //move up
    //find all steiner points between a and best_pos
    Vertex_flute_ptr cur = &a;
    while (test(cur->c.y, best_pos)) {
        Vertex_flute_ptr find = *(cur->neighbor.begin());
        for (vector<Vertex_flute_ptr>::iterator nei = cur->neighbor.begin() + 1; nei != cur->neighbor.end(); ++nei)
            if (test(find->c.y, (*nei)->c.y))
                find = *nei;
        cur = find;
        st_pt.push_back(cur);
    }
    //exchange neighb
    for (int i = 0; i < (int) (st_pt.size()); ++i) {
        if (test(st_pt[i]->c.y, best_pos)) {
            int ind1 = 0;
            for (ind1 = 0;; ++ind1) {
                if (!((a.neighbor[ind1]->c == b.c) || (a.neighbor[ind1]->c == st_pt[i]->c))) {
                    break;
                }
            }
            int ind2 = 0;
            for (ind2 = 0;; ++ind2)
                if (test(st_pt[i]->c.y, st_pt[i]->neighbor[ind2]->c.y))
                    break;
            for (int j = 0;; ++j)
                if (a.neighbor[ind1]->neighbor[j] == &a) {
                    a.neighbor[ind1]->neighbor[j] = st_pt[i];
                    break;
                }
            for (int j = 0;; ++j)
                if (st_pt[i]->neighbor[ind2]->neighbor[j] == st_pt[i]) {
                    st_pt[i]->neighbor[ind2]->neighbor[j] = &a;
                    break;
                }
            swap(a.neighbor[ind1], st_pt[i]->neighbor[ind2]);
            a.c.y = st_pt[i]->c.y;
        } else if (st_pt[i]->c.x == a.c.x && st_pt[i]->c.y == best_pos)
            overlap_a = st_pt[i];
        else
            break;
    }
    return;
}

void Construct_2d_tree::move_edge_ver(Vertex_flute& a, int best_pos, Vertex_flute& b, Vertex_flute_ptr& overlap_a, std::function<bool(const int& i, const int& j)> test) {
    //find all steiner points between a and best_pos
    vector<Vertex_flute_ptr> st_pt;
    Vertex_flute_ptr cur = &a;
    while (test(cur->c.x, best_pos)) {
        Vertex_flute_ptr find = *(cur->neighbor.begin());
        for (vector<Vertex_flute_ptr>::iterator nei = cur->neighbor.begin() + 1; nei != cur->neighbor.end(); ++nei)
            if (test(find->c.x, (*nei)->c.x))
                find = *nei;
        cur = find;
        st_pt.push_back(cur);
    }
    //exchange neighbor
    for (int i = 0; i < (int) (st_pt.size()); ++i) {
        if (test(st_pt[i]->c.x, best_pos)) {
            int ind1 = 0;
            for (ind1 = 0;; ++ind1)
                if (!((a.neighbor[ind1]->c == b.c) || (a.neighbor[ind1]->c == st_pt[i]->c)))
                    break;
            int ind2 = 0;
            for (ind2 = 0;; ++ind2)
                if (test(st_pt[i]->c.x, st_pt[i]->neighbor[ind2]->c.x))
                    break;
            for (int j = 0;; ++j)
                if (a.neighbor[ind1]->neighbor[j] == &a) {
                    a.neighbor[ind1]->neighbor[j] = st_pt[i];
                    break;
                }
            for (int j = 0;; ++j)
                if (st_pt[i]->neighbor[ind2]->neighbor[j] == st_pt[i]) {
                    st_pt[i]->neighbor[ind2]->neighbor[j] = &a;
                    break;
                }
            swap(a.neighbor[ind1], st_pt[i]->neighbor[ind2]);
            a.c.x = st_pt[i]->c.x;
        } else if (st_pt[i]->c.x == best_pos && st_pt[i]->c.y == a.c.y)
            overlap_a = st_pt[i];
        else
            break;
    }

}

bool Construct_2d_tree::move_edge(Vertex_flute& a, Vertex_flute& b, int best_pos, int dir) {
    auto greater = [&](const int& i,const int& j) {
        return i > j;
    };
    auto less = [&](const int& i,const int& j) {
        return i < j;
    };
    Vertex_flute_ptr overlap_a, overlap_b;

    overlap_a = overlap_b = NULL;
    if (dir == HOR) {
        if (best_pos > a.c.y) {	//move up
            //find all steiner points between a and best_pos
            move_edge_hor(a, best_pos, b, overlap_a, less);
            move_edge_hor(b, best_pos, a, overlap_b, less);
            a.c.y = best_pos;
            b.c.y = best_pos;
        } else {	//move down
            move_edge_hor(a, best_pos, b, overlap_a, greater);
            move_edge_hor(b, best_pos, a, overlap_b, greater);
            a.c.y = best_pos;
            b.c.y = best_pos;
        }
    } else {	//VER
        if (best_pos > a.c.x) {	//move right
            //find all steiner points between a and best_pos
            move_edge_ver(a, best_pos, b, overlap_a, less);
            move_edge_ver(b, best_pos, a, overlap_b, less);
            a.c.x = best_pos;
            b.c.x = best_pos;
        } else {	//move left
            move_edge_ver(a, best_pos, b, overlap_a, greater);
            move_edge_ver(b, best_pos, a, overlap_b, greater);
            a.c.x = best_pos;
            b.c.x = best_pos;
        }
    }
    bool ret = false;
    if (overlap_a != NULL) {
        merge_vertex(*overlap_a, a);
        ret = true;
    }
    if (overlap_b != NULL)
        merge_vertex(*overlap_b, b);

    return ret;
}

void Construct_2d_tree::traverse_tree(double& ori_cost, std::vector<Vertex_flute>& vertex_fl) {
    double cur_cost, tmp_cost, best_cost;
    int best_pos = 0;

    for (Vertex_flute& node : vertex_fl) {

        node.visit = 1;

        if (node.type == STEINER && node.neighbor.size() <= 3) {	//remove3!!!

            for (Vertex_flute_ptr it : node.neighbor)
                if (it->visit == 0 && (it->type == STEINER && it->neighbor.size() <= 3))	//!!!remove3!!
                        {
                    int low = 0;
                    int high = INT_MAX;
                    Vertex_flute& a = node;
                    Vertex_flute& b = *it;

                    if (it->c.y == node.c.y) {	//horizontal edge (vertical shifting)

                        find_saferange(a, b, &low, &high, HOR);		//compute safe range
                        best_cost = ori_cost;
                        cur_cost = (ori_cost) - compute_L_pattern_cost(a.c, b.c, -1);
                        for (int pos = low; pos <= high; ++pos) {
                            tmp_cost = cur_cost + compute_L_pattern_cost(Coordinate_2d { a.c.x, pos }, Coordinate_2d { b.c.x, pos }, -1);
                            if (tmp_cost < best_cost) {
                                best_cost = tmp_cost;
                                best_pos = pos;
                            }
                        }
                        if (best_cost < ori_cost) {	//edge need shifting

                            move_edge(a, b, best_pos, HOR);
                            //move to best position,exchange steiner points if needed
                            ori_cost = best_cost;
                        }
                    } else if (it->c.x == node.c.x)	//vertical edge (horizontal shifting)
                            {
                        find_saferange(a, b, &low, &high, VER);		//compute safe range
                        best_cost = ori_cost;
                        cur_cost = (ori_cost) - compute_L_pattern_cost(a.c, b.c, -1);
                        for (int pos = low; pos <= high; ++pos) {
                            tmp_cost = cur_cost + compute_L_pattern_cost(Coordinate_2d { pos, a.c.y }, Coordinate_2d { pos, b.c.y }, -1);
                            if (tmp_cost < best_cost) {
                                best_cost = tmp_cost;
                                best_pos = pos;
                            }
                        }
                        if (best_cost < ori_cost)	//edge need shifting
                                {
                            move_edge(a, b, best_pos, VER);
                            //move to best position,exchange steiner points if needed
                            ori_cost = best_cost;
                        }
                    } else
                        continue;
                }
        }
    }
}

void Construct_2d_tree::dfs_output_tree(Vertex_flute& node, Tree &t) {
    node.visit = 1;
    t.branch[t.number].x = node.c.x;
    t.branch[t.number].y = node.c.y;
    node.index = t.number;
    (t.number) += 1;
    for (Vertex_flute_ptr it : node.neighbor) {
        if ((it->visit == 0) && (it->type != DELETED)) {
            dfs_output_tree(*it, t);                  //keep tracing deeper vertices
            t.branch[it->index].n = node.index;    //make parent of target vertex point to current vertex
        }
    }
}

void Construct_2d_tree::edge_shifting(Tree& t) {

    double ori_cost = 0;            // the original cost without edge shifting
    std::vector<Vertex_flute> vertex_fl;
//Create vertex
    int degSize = 2 * t.deg - 2;
    vertex_fl.reserve(degSize);
    for (int i = 0; i < t.deg; ++i) {
        vertex_fl.emplace_back((int) t.branch[i].x, (int) t.branch[i].y, PIN);

    }
    for (int i = t.deg; i < degSize; ++i) {
        vertex_fl.emplace_back((int) t.branch[i].x, (int) t.branch[i].y, STEINER);

    }

//Create edge
    for (int i = 0; i < degSize; ++i) {

        Vertex_flute_ptr vi = &vertex_fl[i];
        Vertex_flute_ptr vn = &vertex_fl[t.branch[i].n];
//skip the vertex if it is the same vertex with its neighbor
        if ((vi->c == vn->c))
            continue;
        vi->neighbor.push_back(vn);
        vn->neighbor.push_back(vi);
//compute original tree cost
        ori_cost += compute_L_pattern_cost(vi->c, vn->c, -1);
    }
    std::sort(vertex_fl.begin(), vertex_fl.end(), [&](const Vertex_flute& a, const Vertex_flute& b) {return Vertex_flute::comp_vertex_fl( a, b);});

    for (int i = 0, j = 1; j < degSize; ++j) {
        Vertex_flute& vi = vertex_fl[i];
        Vertex_flute& vj = vertex_fl[j];
        if ((vi.c == vj.c))	//j is redundant
        {
            vj.type = DELETED;
            for (Vertex_flute_ptr it : vj.neighbor) {
                if ((it->c != vi.c))	//not i,add 0430
                {
                    vi.neighbor.push_back(it);
                    for (int k = 0; k < (int) it->neighbor.size(); ++k) {
                        if (it->neighbor[k]->c == vi.c) {
                            it->neighbor[k] = &vi;	//modify it's neighbor to i
                            break;
                        }
                    }
                }
            }
        } else
            i = j;
    }

    for (Vertex_flute& fl : vertex_fl) {
        fl.visit = 0;
    }
    traverse_tree(ori_cost, vertex_fl);	// dfs to find 2 adjacent steiner points(h or v edge) and do edge_shifhting

//Output the result (2-pin lists) to a Tree structure in DFS order
//1. make sure every 2-pin list have not been visited
    for (Vertex_flute& fl : vertex_fl) {
        fl.visit = 0;
    }
    t.number = 0;

//2. begin to out put the 2-pin lists to a Tree strucuture
    dfs_output_tree(vertex_fl[0], t);
    t.branch[0].n = 0;	//because neighbor of root is not assign in dfs_output_tree()

}
//=====================end edge shifting=============================

/* sort by bounding box size */
void Construct_2d_tree::output_2_pin_list() {

    std::sort(two_pin_list.begin(), two_pin_list.end(), [&](Two_pin_element_2d &a, Two_pin_element_2d &b) {
        return Two_pin_element_2d::comp_2pin_net_from_path(a,b);
    });
}

/*stage 1: construct 2d steiner tree
 output 2-pin list to stage2
 return max_overflow;
 */

Construct_2d_tree::Construct_2d_tree(RoutingParameters& routingparam, ParameterSet& param, RoutingRegion& rr) :
//
        //
        parameter_set { param }, //
        routing_parameter { routingparam }, //
        bboxRouteStateMap { rr.get_gridx(), rr.get_gridy() }, //
        rr_map { rr }, //
        post_processing { *this }, //
        congestion { rr.get_gridx(), rr.get_gridy() } {

    mazeroute_in_range = NULL;

    /***********************
     * Global Variable End
     * ********************/

    cur_iter = -1;                  // current iteration ID.
//edgeIterCounter = new EdgeColorMap<int>(rr_map.get_gridx(), rr_map.get_gridy(), -1);

    readLUT();                      // Function in flute, function: unknown

    /* TroyLee: NetDirtyBit Counter */
    NetDirtyBit = vector<bool>(rr_map.get_netNumber(), true);
    /* TroyLee: End */

    if (routing_parameter.get_monotonic_en()) {
        //allocate_monotonic();           // Allocate the memory for storing the data while searching monotonic path
// 1. A 2D array that stores max congestion
// 2. A 2D array that stores parent (x,y) during finding monotonic path
    }

    gen_FR_congestion_map();        // Generate congestion map by flute, then route all nets by L-shap pattern routing with
// congestion information from this map. After that, apply edge shifting to the result
// to get the initial solution.

    cal_total_wirelength();         // The report value is the sum of demand on every edge

    RangeRouter rangeRouter(*this);

    Route_2pinnets route_2pinnets(*this, rangeRouter);

    route_2pinnets.allocate_gridcell();            //question: I don't know what this for. (jalamorm, 07/10/31)

//Make a 2-pin net list without group by net
    for (int i = 0; i < rr_map.get_netNumber(); ++i) {
        for (int j = 0; j < (int) net_2pin_list[i].size(); ++j) {
            two_pin_list.push_back((*net_2pin_list[i])[j]);
        }
    }

    route_2pinnets.reallocate_two_pin_list();
    mazeroute_in_range = new Multisource_multisink_mazeroute(*this);

    int cur_overflow = -1;
    used_cost_flag = HISTORY_COST;
    BOXSIZE_INC = routing_parameter.get_init_box_size_p2();

    for (cur_iter = 1, done_iter = cur_iter; cur_iter <= routing_parameter.get_iteration_p2(); ++cur_iter, done_iter = cur_iter)   //do n-1 times
            {
        std::cout << "\033[31mIteration:\033[m " << cur_iter << std::endl;

        factor = (1.0 - exp(-5 * exp(-(0.1 * cur_iter))));

        WL_Cost = factor;
        via_cost = static_cast<int>(4 * factor);

#ifdef MESSAGE
        cout << "Parameters - Factor: " << factor
        << ", Via_Cost: " << via_cost
        << ", Box Size: " << BOXSIZE_INC + cur_iter - 1 << endl;
#endif

        pre_evaluate_congestion_cost();

//route_all_2pin_net(false);
        route_2pinnets.route_all_2pin_net();

        cur_overflow = cal_max_overflow();
        cal_total_wirelength();

        if (cur_overflow == 0)
            break;

        route_2pinnets.reallocate_two_pin_list();

#ifdef MESSAGE
        printMemoryUsage("Memory Usage:");
#endif

        if (cur_overflow <= routing_parameter.get_overflow_threshold()) {
            break;
        }
        BOXSIZE_INC += routing_parameter.get_box_size_inc_p2();
    }

    output_2_pin_list();    //order:bbox�p

#ifdef MESSAGE
    cout<<"================================================================"<<endl;
    cout<<"===                   Enter Post Processing                  ==="<<endl;
    cout<<"================================================================"<<endl;
#endif
    post_processing.process(route_2pinnets);
}

inline void Construct_2d_tree::printMemoryUsage(const char* msg) {
    std::cout << msg << std::endl;
//for print out memory usage
    std::ifstream mem("/proc/self/status");
    std::string memory;
    for (unsigned i = 0; i < 13; ++i) {
        getline(mem, memory);
        if (i > 10) {
            std::cout << memory << std::endl;
        }
    }
}
