
namespace FlaxEngine
{
    partial struct WorldEntity
    {
        /// <summary>
        /// Invalid way to create entity guard it
        /// </summary>
        /// <exception cref="System.Exception"></exception>
        public WorldEntity()
        {
            throw new System.Exception("[INVALID API USAGE] Cant create entity this way call World.CreateWorldEntity() insted");
        }
        /// <summary>
        /// Returns <see cref="WorldSector"/> with contains <see cref="WorldEntity"/>
        /// </summary>
        public readonly WorldSector Sector => World.GetWorldSectorFromPtr(m_Sector);

        /// <summary>
        /// the ID of the  <see cref="WorldEntity"/>
        /// </summary>
        public readonly ulong ID => m_ID;

        /// <summary>
        /// cheak if entity has a component
        /// </summary>
        /// <typeparam name="T"></typeparam>
        /// <returns>if entity has component</returns>
        public bool HasComponent<T>() where T : Component
        {
            return World.HasComponent(this, typeof(T));
        }

        /// <summary>
        /// gets component
        /// </summary>
        /// <typeparam name="T">component type</typeparam>
        /// <returns>component</returns>
        public T GetComponent<T>() where T : Component, new()
        {
            return (T)World.GetComponent(this, typeof(T));
        }

        /// <summary>
        /// gets or adds component
        /// </summary>
        /// <typeparam name="T">component type</typeparam>
        /// <returns>new or existing component</returns>
        public T GetOrAddComponent<T>() where T : Component
        {
            return (T)World.GetOrAddComponent(this, typeof(T));
        }

        /// <summary>
        /// adds component
        /// </summary>
        /// <typeparam name="T">type</typeparam>
        /// <returns>new component</returns>
        public T AddComponent<T>() where T : Component
        {
            return (T)World.AddComponent(this, typeof(T));
        }
        /// <summary>
        /// 
        /// </summary>
        /// <typeparam name="T"></typeparam>
        public void DestoryComponent<T>() where T : Component
        {
            World.DestoryComponent(this, typeof(T));
        }
        /// <summary>
        /// 
        /// </summary>
        /// <returns></returns>
        public bool IsValid()
        {
            return World.IsEntityValid(this);
        }

        /// <summary>
        /// Ceaks if entity is valid
        /// </summary>
        /// <param name="worldEntity"></param>
        public static explicit operator bool(WorldEntity worldEntity)
        {
            return worldEntity.IsValid();
        }
    }
}
