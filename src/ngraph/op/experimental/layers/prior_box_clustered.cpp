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

#include "prior_box_clustered.hpp"

#include "ngraph/op/constant.hpp"

using namespace std;
using namespace ngraph;

op::PriorBoxClustered::PriorBoxClustered(const shared_ptr<Node>& layer_shape,
                                         const shared_ptr<Node>& image_shape,
                                         const size_t num_priors,
                                         const std::vector<float>& widths,
                                         const std::vector<float>& heights,
                                         const bool clip,
                                         const float step_widths,
                                         const float step_heights,
                                         const float offset,
                                         const std::vector<float>& variances)
    : Op("PriorBoxClustered", check_single_output_args({layer_shape, image_shape}))
    , m_num_priors(num_priors)
    , m_widths(widths)
    , m_heights(heights)
    , m_clip(clip)
    , m_step_widths(step_widths)
    , m_step_heights(step_heights)
    , m_offset(offset)
    , m_variances(variances)
{
    constructor_validate_and_infer_types();
}

void op::PriorBoxClustered::validate_and_infer_types()
{
    // shape node should have integer data type. For now we only allow i64
    auto layer_shape_et = get_input_element_type(0);
    NODE_VALIDATION_CHECK(this,
                          layer_shape_et.compatible(element::Type_t::i64),
                          "layer shape input must have element type i64, but has ",
                          layer_shape_et);

    auto image_shape_et = get_input_element_type(1);
    NODE_VALIDATION_CHECK(this,
                          image_shape_et.compatible(element::Type_t::i64),
                          "image shape input must have element type i64, but has ",
                          image_shape_et);

    auto layer_shape_rank = get_input_partial_shape(0).rank();
    auto image_shape_rank = get_input_partial_shape(1).rank();
    NODE_VALIDATION_CHECK(this,
                          layer_shape_rank.compatible(image_shape_rank),
                          "layer shape input rank ",
                          layer_shape_rank,
                          " must match image shape input rank ",
                          image_shape_rank);

    NODE_VALIDATION_CHECK(this,
                          m_widths.size() == m_num_priors,
                          "Num_priors ",
                          m_num_priors,
                          " doesn't match size of widths vector ",
                          m_widths.size());

    NODE_VALIDATION_CHECK(this,
                          m_heights.size() == m_num_priors,
                          "Num_priors ",
                          m_num_priors,
                          " doesn't match size of heights vector ",
                          m_heights.size());

    set_input_is_relevant_to_shape(0);

    if (auto const_shape = dynamic_pointer_cast<op::Constant>(get_argument(0)))
    {
        NODE_VALIDATION_CHECK(this,
                              shape_size(const_shape->get_shape()) == 2,
                              "Layer shape must have rank 2",
                              const_shape->get_shape());

        auto layer_shape = static_cast<const int64_t*>(const_shape->get_data_ptr());
        // {Prior boxes, variances-adjusted prior boxes}
        set_output_type(
            0, element::f32, Shape{2, 4 * layer_shape[0] * layer_shape[1] * m_num_priors});
    }
    else
    {
        set_output_type(0, element::f32, PartialShape::dynamic());
    }
}

shared_ptr<Node> op::PriorBoxClustered::copy_with_new_args(const NodeVector& new_args) const
{
    check_new_args_count(this, new_args);
    return make_shared<PriorBoxClustered>(new_args.at(0),
                                          new_args.at(1),
                                          m_num_priors,
                                          m_widths,
                                          m_heights,
                                          m_clip,
                                          m_step_widths,
                                          m_step_heights,
                                          m_offset,
                                          m_variances);
}
