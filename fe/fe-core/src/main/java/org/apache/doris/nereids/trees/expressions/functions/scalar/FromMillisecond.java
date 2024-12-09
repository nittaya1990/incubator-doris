// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.doris.nereids.trees.expressions.functions.scalar;

import org.apache.doris.catalog.FunctionSignature;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.functions.ExplicitlyCastableSignature;
import org.apache.doris.nereids.trees.expressions.functions.PropagateNullableOnDateLikeV2Args;
import org.apache.doris.nereids.trees.expressions.shape.BinaryExpression;
import org.apache.doris.nereids.trees.expressions.visitor.ExpressionVisitor;
import org.apache.doris.nereids.types.BigIntType;
import org.apache.doris.nereids.types.DateTimeV2Type;

import com.google.common.base.Preconditions;
import com.google.common.collect.ImmutableList;

import java.util.List;

/**
 * ScalarFunction 'from_millisecond'.
 */
public class FromMillisecond extends ScalarFunction
        implements BinaryExpression, ExplicitlyCastableSignature, PropagateNullableOnDateLikeV2Args {
    public static final DateTimeV2Type MillisecondDateTimeV2 = DateTimeV2Type.of(3);
    private static final List<FunctionSignature> SIGNATURES = ImmutableList.of(
            FunctionSignature.ret(MillisecondDateTimeV2).args(BigIntType.INSTANCE));

    public FromMillisecond(Expression arg0) {
        super("from_millisecond", arg0);
    }

    @Override
    public FromMillisecond withChildren(List<Expression> children) {
        Preconditions.checkArgument(children.size() == 1);
        return new FromMillisecond(children.get(0));
    }

    @Override
    public List<FunctionSignature> getSignatures() {
        return SIGNATURES;
    }

    @Override
    public <R, C> R accept(ExpressionVisitor<R, C> visitor, C context) {
        return visitor.visitScalarFunction(this, context);
    }
}
