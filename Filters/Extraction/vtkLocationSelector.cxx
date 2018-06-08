/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkLocationSelector.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkLocationSelector.h"

#include "vtkDataSetAttributes.h"
#include "vtkInformation.h"
#include "vtkNew.h"
#include "vtkObjectFactory.h"
#include "vtkPoints.h"
#include "vtkSMPTools.h"
#include "vtkSelectionNode.h"
#include "vtkSignedCharArray.h"
#include "vtkStaticCellLocator.h"
#include "vtkStaticPointLocator.h"
#include "vtkUnstructuredGrid.h"

#include <cassert>

class vtkLocationSelector::vtkInternals
{
protected:
  vtkSmartPointer<vtkDataArray> SelectionList;
  double SearchRadius;

public:
  vtkInternals(vtkDataArray* selList, double searchRadius)
    : SelectionList(selList)
    , SearchRadius(searchRadius)
  {
  }
  virtual ~vtkInternals() {}
  virtual bool Execute(vtkDataSet* dataset, vtkSignedCharArray* insidednessArray) = 0;
};

class vtkLocationSelector::vtkInternalsForPoints : public vtkLocationSelector::vtkInternals
{
public:
  vtkInternalsForPoints(vtkDataArray* selList, double searchRadius)
    : vtkInternals(selList, searchRadius)
  {
  }

  bool Execute(vtkDataSet* dataset, vtkSignedCharArray* insidednessArray) override
  {
    const vtkIdType numPoints = dataset->GetNumberOfPoints();
    if (numPoints <= 0)
    {
      return false;
    }

    vtkSmartPointer<vtkStaticPointLocator> locator;

    if (dataset->IsA("vtkPointSet"))
    {
      locator = vtkSmartPointer<vtkStaticPointLocator>::New();
      locator->SetDataSet(dataset);
      locator->Update();
    }

    std::fill_n(insidednessArray->GetPointer(0), numPoints, static_cast<char>(0));
    const double radius = this->SearchRadius;

    // Find points closest to each point in the locations of interest.
    vtkIdType numLocations = this->SelectionList->GetNumberOfTuples();
    for (vtkIdType locationId = 0; locationId < numLocations; ++locationId)
    {
      double location[3], dist2;
      this->SelectionList->GetTuple(locationId, location);

      vtkIdType ptId = -1;
      if (locator)
      {
        ptId = locator->FindClosestPointWithinRadius(radius, location, dist2);
      }
      else
      {
        ptId = dataset->FindPoint(location);
        if (ptId >= 0)
        {
          double *x = dataset->GetPoint(ptId);
          double distance = vtkMath::Distance2BetweenPoints(x, location);
          if (distance > radius*radius)
          {
            ptId = -1;
          }
        }
      }

      if (ptId >= 0)
      {
        insidednessArray->SetTypedComponent(ptId, 0, 1);
      }
    }

    insidednessArray->Modified();
    return true;
  }
};

class vtkLocationSelector::vtkInternalsForCells : public vtkLocationSelector::vtkInternals
{
public:
  vtkInternalsForCells(vtkDataArray* selList, double searchRadius)
    : vtkInternals(selList, searchRadius)
  {
  }

  bool Execute(vtkDataSet* dataset, vtkSignedCharArray* insidednessArray) override
  {
    vtkNew<vtkStaticCellLocator> cellLocator;
    cellLocator->SetDataSet(dataset);
    cellLocator->Update();

    const auto numLocations = this->SelectionList->GetNumberOfTuples();
    const auto numCells = insidednessArray->GetNumberOfTuples();
    std::fill_n(insidednessArray->GetPointer(0), numCells, static_cast<char>(0));
    for (vtkIdType cc = 0; cc < numLocations; ++cc)
    {
      double coords[3];
      this->SelectionList->GetTuple(cc, coords);
      auto cid = cellLocator->FindCell(coords);
      if (cid >= 0 && cid < numCells)
      {
        insidednessArray->SetValue(cid, 1);
      }
    }
    insidednessArray->Modified();
    return true;
  }
};

vtkStandardNewMacro(vtkLocationSelector);
//----------------------------------------------------------------------------
vtkLocationSelector::vtkLocationSelector()
  : Internals(nullptr)
{
}

//----------------------------------------------------------------------------
vtkLocationSelector::~vtkLocationSelector()
{
}

//----------------------------------------------------------------------------
void vtkLocationSelector::Initialize(vtkSelectionNode* node, const std::string& insidednessArrayName)
{
  this->Superclass::Initialize(node, insidednessArrayName);

  this->Internals.reset();

  auto selectionList = vtkDataArray::SafeDownCast(node->GetSelectionList());
  if (!selectionList || selectionList->GetNumberOfTuples() == 0)
  {
    // empty selection list, nothing to do.
    return;
  }

  if (selectionList->GetNumberOfComponents() != 3)
  {
    vtkErrorMacro("Only 3-d locations are current supported.");
    return;
  }

  if (node->GetContentType() != vtkSelectionNode::LOCATIONS)
  {
    vtkErrorMacro("vtkLocationSelector only supported vtkSelectionNode::LOCATIONS. `"
      << node->GetContentType() << "` is not supported.");
    return;
  }

  const int fieldType = node->GetFieldType();
  const int assoc = vtkSelectionNode::ConvertSelectionFieldToAttributeType(fieldType);

  double radius = node->GetProperties()->Has(vtkSelectionNode::EPSILON())
    ? node->GetProperties()->Get(vtkSelectionNode::EPSILON())
    : 0.0;
  switch (assoc)
  {
    case vtkDataObject::FIELD_ASSOCIATION_POINTS:
      this->Internals.reset(new vtkInternalsForPoints(selectionList, radius));
      break;

    case vtkDataObject::FIELD_ASSOCIATION_CELLS:
      this->Internals.reset(new vtkInternalsForCells(selectionList, radius));
      break;

    default:
      vtkErrorMacro(
        "vtkLocationSelector does not support requested field type `" << fieldType << "`.");
      break;
  }
}

//----------------------------------------------------------------------------
void vtkLocationSelector::Finalize()
{
  this->Internals.reset();
}

//----------------------------------------------------------------------------
bool vtkLocationSelector::ComputeSelectedElementsForBlock(vtkDataObject* input,
  vtkSignedCharArray* insidednessArray, unsigned int vtkNotUsed(compositeIndex),
  unsigned int vtkNotUsed(amrLevel), unsigned int vtkNotUsed(amrIndex))
{
  assert(input != nullptr && insidednessArray != nullptr);
  vtkDataSet* ds = vtkDataSet::SafeDownCast(input);
  return (this->Internals != nullptr && ds != nullptr) ? this->Internals->Execute(ds, insidednessArray)
                                                       : false;
}

//----------------------------------------------------------------------------
void vtkLocationSelector::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os, indent);
}
