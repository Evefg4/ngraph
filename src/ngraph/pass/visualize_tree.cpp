//*****************************************************************************
// Copyright 2017-2019 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//*****************************************************************************

#include <fstream>

#include "ngraph/function.hpp"
#include "ngraph/graph_util.hpp"
#include "ngraph/node.hpp"
#include "ngraph/op/get_output_element.hpp"
#include "ngraph/pass/pass.hpp"
#include "ngraph/pass/visualize_tree.hpp"
#include "ngraph/util.hpp"

using namespace ngraph;
using namespace std;

#define TI(x) type_index(typeid(x))

//
// As we are visualizing the graph, we will make some tweaks to the generated dot file to make
// routing more tractable for Graphviz as well as (hopefully) more legible for the user.
//
// NOTE: It's possible, even likely, that better algorithms are available here. I just tried a
// few different things without doing much research, and this seemed to work well. Please feel
// free to improve on this. --amprocte
//
// -----------------
//
// The first tweak is to trim edges that, intuitively speaking, have long "skip distance". For
// example:
//
// [Actual Graph Structure]      [Visualization]
//    n0                             n0
//    | \                            |  \
//    n1 \                           n1  [to n50]
//    |   |                          |
//    n2  |                          n2
//    |   |                          |
//    n3  |                          n3
//    |   |                          |
//   ...  |                         ...  [from n0]
//    |  /                           |  /
//   n50                            n50
//
// This is useful for training graphs especially, which tend to have very long feed-forward edges
// for intermediate values from fprop being stored for later reuse in the bprop phase.
//
// Efficiently detecting a "long skip" is a bit tricky. We want to come up with a metric that is
// reasonably fast to compute, but does not result in cuts that will split the graph into multiple
// components. The heuristic we are using for the jump distance between n and m is the maximum
// difference in maximum path length from n and m to any result node that is reachable from both
// n and m (or 0, if no such result node exists). Not sure if this is mathematically *guaranteed*
// not to split graph components, but it seems to work well in practice.
//
// Formally:
//
// Compute-Heights-Above-Each-Parameter(N):
//    Inputs: nodes N; define R={n in N | n is a Result node}
//    Output: height_maps: map from N to (map from R to int)
//
//    height_maps is initially empty
//
//    for each r in R:
//        Insert into height_map the map {r -> 1}
//
//    for each n in N in reverse topological ("results-first") order:
//        for each user m of n:
//            for each r in height_maps[m].keys:
//                height_maps[n][r] := max(height_maps[n][r], height_maps[m][r]+1)
//
// Jump-Distance(n,m,height_maps):
//     Inputs: n (source node), m (destination node), height_maps (pre-computed above)
//     Output: jump_distance: int
//
//     jump_distance := 0
//
//     for each r in height_maps[n].keys:
//         if r is in height_maps[m].keys:
//             jump_distance := max(jump_distance, abs(height_maps[n][r] - height_maps[m][r]))
//
// Later on, if E is an edge from n to m, and Jump-Distance(n,m,height_map) > K (where K is kind
// of arbitrary but currently set to 20), we will "cut" the edge as illustrated above.
//
// -----------------
//
// The second tweak aims to eliminate routing pressure from nodes that have large outdegree and
// are connected to many otherwise-distant places in the graph. For this, the only thing we are
// doing at the moment is to "float" Parameter and Constant nodes. This means that rather than
// visualizing them as a single node (which might have very large outdegree as in, e.g., a
// learning rate parameter being fed to many different places), we make a "copy" of the node at
// each occurrence site (with a dashed outline).
//
// NOTE: This tweak could probably be extended to float other kinds of nodes with high out-degree.
// (This situation is likely to arise after constant subexpression elimination.) Here one has to
// be careful to avoid splitting the components. I have some rough ideas on how this could be
// dealt with, but have not had time to implement them yet. --amprocte
//
class HeightMap
{
public:
    HeightMap() {}
    HeightMap(std::set<Node*> initials)
    {
        for (auto& n : initials)
        {
            m_heights[n] = 0;
        }
    }
    void absorb(const HeightMap& other)
    {
        for (auto& p : other.m_heights)
        {
            auto k = p.first;
            auto v = p.second;
            m_heights[k] = std::max(m_heights[k], v + 1);
        }
    }
    int64_t max_jump_to(const HeightMap& target)
    {
        int64_t result = 0;
        for (auto& p : m_heights)
        {
            auto k = p.first;
            auto v = p.second;
            if (target.m_heights.count(k) != 0)
            {
                result = std::max(result, std::abs(target.m_heights.at(k) - v));
            }
        }
        return result;
    }

private:
    std::unordered_map<Node*, int64_t> m_heights;
};

static std::string label_edge(const std::shared_ptr<Node>& src,
                              const std::shared_ptr<Node>& dst,
                              size_t arg_index,
                              int64_t jump_distance)
{
    std::stringstream ss;
    if (getenv("NGRAPH_VISUALIZE_EDGE_LABELS") != nullptr)
    {
        size_t output = 0;
        if (auto goe = dynamic_pointer_cast<op::GetOutputElement>(dst))
        {
            output = goe->get_n();
        }
        stringstream label_edge;
        label_edge << "[label=\" " << output << " -> " << arg_index << " \"]";
        ss << label_edge.str();
    }

    else if (getenv("NGRAPH_VISUALIZE_EDGE_JUMP_DISTANCE") != nullptr)
    {
        if (jump_distance > 1)
        {
            stringstream label_edge;
            label_edge << "[label=\"jump=" << jump_distance << "\"]";
            ss << label_edge.str();
        }
    }
    return ss.str();
}

bool pass::VisualizeTree::run_on_module(vector<shared_ptr<Function>>& functions)
{
    for (shared_ptr<Function> f : functions)
    {
        unordered_map<Node*, HeightMap> height_maps;

        for (auto& node : f->get_ops())
        {
            if (node->description() == "Result")
            {
                height_maps[node.get()] = HeightMap({node.get()});
            }
            else
            {
                height_maps[node.get()] = HeightMap();
            }
        }

        auto nodes = topological_sort(f->get_ops());
        nodes.reverse();

        for (auto& node : nodes)
        {
            for (auto& output : node->outputs())
            {
                for (auto& input : output.get_target_inputs())
                {
                    auto target_node = input.get_node();
                    height_maps[node.get()].absorb(height_maps[target_node]);
                }
            }
        }

        // TODO(amprocte): Maybe find a way to make this tunable.
        const int max_jump_distance = 20;

        size_t fake_node_ctr = 0;

        traverse_nodes(f, [&](shared_ptr<Node> node) {
            size_t arg_index = 0;
            for (auto arg : node->get_arguments())
            {
                size_t jump_distance = height_maps[arg.get()].max_jump_to(height_maps[node.get()]);

                if (arg->description() == "Constant" || arg->description() == "Parameter")
                {
                    auto clone_name = "CLONE_" + to_string(fake_node_ctr);
                    auto color = (arg->description() == "Parameter" ? "blue" : "black");
                    m_ss << "    " << clone_name
                         << "[shape=\"box\" style=\"dashed,filled\" color=\"" << color
                         << "\" fillcolor=\"white\" label=\"" << arg->get_name() << "\"]\n";
                    m_ss << "    " << clone_name << " -> " << node->get_name()
                         << label_edge(arg, node, arg_index, jump_distance) << "\n";
                    fake_node_ctr++;
                }
                else if (jump_distance > max_jump_distance)
                {
                    m_ss << add_attributes(arg);
                    m_ss << add_attributes(node);
                    auto recv_node_name = "RECV_" + to_string(fake_node_ctr);
                    auto send_node_name = "SEND_" + to_string(fake_node_ctr);

                    m_ss << "    " << recv_node_name << "[shape=\"box\" style=\"solid,filled\" "
                                                        "fillcolor=\"#ffcccc\" label=\"Receive["
                         << arg->get_name() << "]\"]\n";
                    m_ss << "    " << send_node_name << "[shape=\"box\" style=\"solid,filled\" "
                                                        "fillcolor=\"#ccffcc\" label=\"Send["
                         << node->get_name() << "]\"]\n";

                    m_ss << "    " << arg->get_name() << " -> " << send_node_name
                         << label_edge(arg, node, arg_index, jump_distance) << "\n";
                    m_ss << "    " << recv_node_name << " -> " << node->get_name()
                         << label_edge(arg, node, arg_index, jump_distance) << "\n";
                    fake_node_ctr++;
                }
                else
                {
                    m_ss << add_attributes(arg);
                    m_ss << add_attributes(node);
                    m_ss << "    " << arg->get_name() << " -> " << node->get_name()
                         << label_edge(arg, node, arg_index, jump_distance) << "\n";
                }
                arg_index++;
            }
        });
    }

    render();

    return false;
}

pass::VisualizeTree::VisualizeTree(const string& file_name, node_modifiers_t nm, bool dot_only)
    : m_name{file_name}
    , m_node_modifiers{nm}
    , m_dot_only(dot_only)
{
}

string pass::VisualizeTree::add_attributes(shared_ptr<Node> node)
{
    string rc;
    if (m_nodes_with_attributes.find(node) == m_nodes_with_attributes.end())
    {
        m_nodes_with_attributes.insert(node);
        rc = get_attributes(node);
    }
    return rc;
}

string pass::VisualizeTree::get_attributes(shared_ptr<Node> node)
{
    vector<string> attributes;
    attributes.push_back("shape=box");

    if (node->is_output())
    {
        attributes.push_back("color=crimson");
        attributes.push_back("penwidth=1.5");
    }
    else
    {
        attributes.push_back("color=black");
    }

    // Construct the label attribute
    {
        stringstream label;
        label << "label=\"" << node->get_name();

        static const char* nvtos = getenv("NGRAPH_VISUALIZE_TREE_OUTPUT_SHAPES");
        if (nvtos != nullptr)
        {
            // The shapes of the Outputs of a multi-output op
            // will be printed for its corresponding `GetOutputElement`s
            label << " " << (node->get_output_size() != 1 ? string("[skipped]")
                                                          : vector_to_string(node->get_shape()));
        }

        static const char* nvtot = getenv("NGRAPH_VISUALIZE_TREE_OUTPUT_TYPES");
        if (nvtot != nullptr)
        {
            // The types of the Outputs of a multi-output op
            // will be printed for its corresponding `GetOutputElement`s
            label << " "
                  << ((node->get_output_size() != 1) ? string("[skipped]")
                                                     : node->get_element_type().c_type_string());
        }

        const Node& n = *node;
        auto eh = m_ops_to_details.find(TI(n));
        if (eh != m_ops_to_details.end())
        {
            eh->second(n, label);
        }
        label << "\"";
        attributes.push_back(label.str());
    }

    if (m_node_modifiers)
    {
        m_node_modifiers(*node, attributes);
    }

    stringstream ss;
    ss << "    " << node->get_name() << " [" << join(attributes, " ") << "]\n";

    return ss.str();
}

string pass::VisualizeTree::get_file_ext()
{
    const char* format = getenv("NGRAPH_VISUALIZE_TREE_OUTPUT_FORMAT");
    if (!format)
    {
        format = "dot";
    }

    if (format[0] == '.')
    {
        format += 1;
    }

    return string(format);
}

void pass::VisualizeTree::render() const
{
    auto dot_file = m_name + ".dot";
    ofstream out(dot_file);
    if (out)
    {
        out << "digraph ngraph\n{\n";
        out << m_ss.str();
        out << "}\n";
        out.close();

        if (!m_dot_only && get_file_ext() != "dot")
        {
#ifndef _WIN32
            stringstream ss;
            ss << "dot -T" << get_file_ext() << " " << dot_file << " -o" << m_name << "."
               << get_file_ext();
            auto cmd = ss.str();
            auto stream = popen(cmd.c_str(), "r");
            if (stream)
            {
                pclose(stream);
            }
#endif
        }
    }
}
