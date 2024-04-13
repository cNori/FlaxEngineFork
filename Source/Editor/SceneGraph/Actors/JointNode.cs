// Copyright (c) 2012-2024 Wojciech Figat. All rights reserved.

using FlaxEngine;

namespace FlaxEditor.SceneGraph.Actors
{
    /// <summary>
    /// Scene tree node for <see cref="Joint"/> actor type.
    /// </summary>
    [HideInEditor]
    public sealed class JointNode : ActorNode
    {
        /// <inheritdoc />
        public JointNode(Actor actor)
        : base(actor)
        {
        }

        /// <inheritdoc />
        public override void PostConvert(ActorNode source)
        {
            base.PostConvert(source);

            if (source.Actor is Joint other)
            {
                // Preserve basic properties when changing joint type
                var joint = (Joint)Actor;
                joint.ConstraintActorA = other.ConstraintActorA;
                joint.LocalConstrainActorA = other.LocalConstrainActorA;

                joint.ConstraintActorB = other.ConstraintActorB;
                joint.LocalConstrainActorB = other.LocalConstrainActorB;

                joint.BreakForce = other.BreakForce;
                joint.BreakTorque = other.BreakTorque;
                joint.EnableAutoAnchor = other.EnableAutoAnchor;
                joint.EnableCollision = other.EnableCollision;
            }
        }
    }
}
