
/*

Copyright (c) 2015, Project OSRM contributors
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

Redistributions of source code must retain the above copyright notice, this list
of conditions and the following disclaimer.
Redistributions in binary form must reproduce the above copyright notice, this
list of conditions and the following disclaimer in the documentation and/or
other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef ROUND_TRIP_BF_HPP
#define ROUND_TRIP_BF_HPP

#include "plugin_base.hpp"

#include "../algorithms/object_encoder.hpp"
#include "../routing_algorithms/tsp_nearest_neighbour.hpp"
#include "../routing_algorithms/tsp_farthest_insertion.hpp"
#include "../routing_algorithms/tsp_brute_force.hpp"
#include "../data_structures/query_edge.hpp"
#include "../data_structures/search_engine.hpp"
#include "../descriptors/descriptor_base.hpp"
#include "../descriptors/json_descriptor.hpp"
#include "../util/json_renderer.hpp"
#include "../util/make_unique.hpp"
#include "../util/string_util.hpp"
#include "../util/timing_util.hpp"
#include "../util/simple_logger.hpp"

#include <osrm/json_container.hpp>

#include <cstdlib>
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <string>
#include <vector>
#include <limits>

template <class DataFacadeT> class RoundTripPluginBF final : public BasePlugin
{
  private:
    std::string descriptor_string;
    DataFacadeT *facade;
    std::unique_ptr<SearchEngine<DataFacadeT>> search_engine_ptr;

  public:
    explicit RoundTripPluginBF(DataFacadeT *facade)
        : descriptor_string("tripBF"), facade(facade)
    {
        search_engine_ptr = osrm::make_unique<SearchEngine<DataFacadeT>>(facade);
    }

    const std::string GetDescriptor() const override final { return descriptor_string; }

    void GetPhantomNodes(const RouteParameters &route_parameters, PhantomNodeArray & phantom_node_vector) {
        const bool checksum_OK = (route_parameters.check_sum == facade->GetCheckSum());

        // find phantom nodes for all input coords
        for (const auto i : osrm::irange<std::size_t>(0, route_parameters.coordinates.size())) {
            // if client hints are helpful, encode hints
            if (checksum_OK && i < route_parameters.hints.size() &&
                !route_parameters.hints[i].empty()) {
                PhantomNode current_phantom_node;
                ObjectEncoder::DecodeFromBase64(route_parameters.hints[i], current_phantom_node);
                if (current_phantom_node.is_valid(facade->GetNumberOfNodes()))
                {
                    phantom_node_vector[i].emplace_back(std::move(current_phantom_node));
                    continue;
                }
            }
            facade->IncrementalFindPhantomNodeForCoordinate(route_parameters.coordinates[i],
                                                            phantom_node_vector[i], 1);
            if (phantom_node_vector[i].size() > 1) {
                phantom_node_vector[i].erase(phantom_node_vector[i].begin());
            }
            BOOST_ASSERT(phantom_node_vector[i].front().is_valid(facade->GetNumberOfNodes()));
        }
    }

    void SplitUnaccessibleLocations(PhantomNodeArray & phantom_node_vector,
                                    std::vector<EdgeWeight> & result_table,
                                    std::vector<unsigned> & component_number_of_locations,
                                    std::vector<PhantomNodeArray> & component_phantom_node_vector,
                                    std::vector<std::vector<EdgeWeight>> & component_table) {

        auto number_of_locations = phantom_node_vector.size();
        auto wrapper = std::make_shared<MatrixGraphWrapper<EdgeWeight>>(result_table, number_of_locations);
        auto empty_restriction = RestrictionMap(std::vector<TurnRestriction>());
        auto empty_vector = std::vector<bool>();
        auto scc = TarjanSCC<MatrixGraphWrapper<EdgeWeight>>(wrapper, empty_restriction, empty_vector);
        scc.run();

        //prepare distance tables, number of locations and phantom node vectors for each SCC
        component_table.reserve(scc.get_number_of_components());
        component_phantom_node_vector.reserve(scc.get_number_of_components());
        for (auto i = 0; i < scc.get_number_of_components(); ++i) {
            std::vector<EdgeWeight> table;
            table.reserve(scc.get_component_size_by_id(i));
            component_table.push_back(table);
            component_number_of_locations.push_back(scc.get_component_size_by_id(i));
            component_phantom_node_vector.push_back(PhantomNodeArray());
        }

        //split the distance table according to the component id of the nodes
        for (auto j = 0; j < result_table.size(); ++j) {
            auto from = j / number_of_locations;
            auto to = j % number_of_locations;
            auto comp_id = scc.get_component_id(from);
            if (comp_id == scc.get_component_id(to)) {
                component_table[comp_id].push_back(result_table[j]);
            }
            if (from == to){
                component_phantom_node_vector[comp_id].push_back(phantom_node_vector[from]);
            }
        }
    }

    void SetLocPermutationOutput(const std::vector<int> & loc_permutation, osrm::json::Object & json_result){
        osrm::json::Array json_loc_permutation;
        json_loc_permutation.values.insert(json_loc_permutation.values.end(), loc_permutation.begin(), loc_permutation.end());
        json_result.values["loc_permutation"] = json_loc_permutation;
    }

    void SetDistanceOutput(const int distance, osrm::json::Object & json_result) {
        json_result.values["distance"] = distance;
    }

    void SetRuntimeOutput(const float runtime, osrm::json::Object & json_result) {
        json_result.values["runtime"] = runtime;
    }

    void SetGeometry(const RouteParameters &route_parameters, const InternalRouteResult & min_route, osrm::json::Object & json_result) {
        // return geometry result to json
        std::unique_ptr<BaseDescriptor<DataFacadeT>> descriptor;
        descriptor = osrm::make_unique<JSONDescriptor<DataFacadeT>>(facade);

        descriptor->SetConfig(route_parameters);
        descriptor->Run(min_route, json_result);
    }


    int HandleRequest(const RouteParameters &route_parameters,
                      osrm::json::Object &json_result) override final
    {
        // check if all inputs are coordinates
        if (!check_all_coordinates(route_parameters.coordinates)) {
            return 400;
        }

        PhantomNodeArray phantom_node_vector(route_parameters.coordinates.size());
        GetPhantomNodes(route_parameters, phantom_node_vector);

        // compute the distance table of all phantom nodes
        const std::shared_ptr<std::vector<EdgeWeight>> result_table =
            search_engine_ptr->distance_table(phantom_node_vector);
        if (!result_table){
            return 400;
        }

        if (route_parameters.coordinates.size() < 11) {
            const auto maxint = std::numeric_limits<int>::max();
            if (*std::max_element(result_table->begin(), result_table->end()) == maxint) {
                std::vector<unsigned> component_number_of_locations;
                std::vector<PhantomNodeArray> component_phantom_node_vector;
                std::vector<std::vector<EdgeWeight>> component_table;
                SplitUnaccessibleLocations(phantom_node_vector, *result_table, component_number_of_locations, component_phantom_node_vector, component_table);

                std::unique_ptr<BaseDescriptor<DataFacadeT>> descriptor;
                descriptor = osrm::make_unique<JSONDescriptor<DataFacadeT>>(facade);
                descriptor->SetConfig(route_parameters);



                TIMER_START(tsp);
                auto min_dist = 0;
                for(auto k = 0; k < component_table.size(); ++k) {
                    if (component_number_of_locations[k] > 1) {
                        auto number_of_locations = component_number_of_locations[k];
                        InternalRouteResult min_route;
                        std::vector<int> min_loc_permutation(number_of_locations, -1);
                        //########################### BRUTE FORCE ####################################//
                        osrm::tsp::BruteForceTSP(component_phantom_node_vector[k], component_table[k], min_route, min_loc_permutation);
                        search_engine_ptr->shortest_path(min_route.segment_end_coordinates, route_parameters.uturns, min_route);
                        min_dist += min_route.shortest_path_length;

                        SetLocPermutationOutput(min_loc_permutation, json_result);
                        // SetGeometry(route_parameters, min_route, json_result);
                        descriptor->Run(min_route, json_result);
                    }
                }
                TIMER_STOP(tsp);

                SetRuntimeOutput(TIMER_MSEC(tsp), json_result);
                SetDistanceOutput(min_dist, json_result);
            } else {
                auto number_of_locations = phantom_node_vector.size();
                InternalRouteResult min_route;
                std::vector<int> min_loc_permutation(number_of_locations, -1);
                //########################### BRUTE FORCE ####################################//
                TIMER_START(tsp);
                osrm::tsp::BruteForceTSP(phantom_node_vector, *result_table, min_route, min_loc_permutation);
                search_engine_ptr->shortest_path(min_route.segment_end_coordinates, route_parameters.uturns, min_route);
                TIMER_STOP(tsp);

                BOOST_ASSERT(min_route.segment_end_coordinates.size() == route_parameters.coordinates.size());

                SetLocPermutationOutput(min_loc_permutation, json_result);
                SetDistanceOutput(min_route.shortest_path_length, json_result);
                SetRuntimeOutput(TIMER_MSEC(tsp), json_result);
                SetGeometry(route_parameters, min_route, json_result);
            }
        } else {
            SetRuntimeOutput(-1, json_result);
            SetDistanceOutput(-1, json_result);
        }
        return 200;
    }

};

#endif // ROUND_TRIP_BF_HPP
