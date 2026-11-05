# engine/voxel

The shared voxel world's building blocks, as plain data: bricks of 8x8x8
voxels (occupancy, colour, normal), voxelizing triangles into a brick, and
the camera-centred clipmap that says which bricks each level holds.

`render` mirrors bricks on the GPU (far geometry, GI); `world` will own
edits (destruction carving the voxel copy, N4). This module knows nothing
about meshes, the scene or the GPU.
